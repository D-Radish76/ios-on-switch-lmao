// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#include "raw_private_comp.h"
#include <os/log.h>
#include <algorithm>
#include <cstring>
#include "raw_gpu.h"

namespace raw {
namespace {

// `from`'s texels into `to`, the same layout on other memory, by the copy engine: each layer, and a multisampled image
// as the grid of its samples
void copyImages(BlitEncoder &b, const Texture &from, const Texture &to)
{
    Texture a = from, c = to;
    uint32_t sx, sy;
    sampleGrid(from.samples, sx, sy);
    for (Texture *grid : {&a, &c}) {
        grid->width <<= sx;
        grid->height <<= sy;
        grid->samples = 1;
    }
    for (uint32_t layer = 0; layer < std::max(from.layers, 1u); layer++)
        b.copyTexels(a, 0, 0, layer, c, 0, 0, layer, a.width, a.height);
}

}  // namespace

bool Device::takeCompressed(uint64_t bytes, uint32_t kind, Bo &bo)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probePlainPrivate.load(std::memory_order_relaxed) || probePlainMemory.load(std::memory_order_relaxed))
        return false;
#endif
    // its own whole comptag lines: a private texture's memory is its alone and never grows, as a backing's class does
    const uint64_t need = compBytes(bytes);
    auto it = comp_.fit(need, kind);
    if (it == comp_.ready.end()) {
        compAsk({need, kind});
        return false;
    }
    bo = it->bo;
    comp_.ready.erase(it);
    return true;
}

void Device::noteCompressed(const TextureAsk &ask, const Bo &bo, uint64_t bytes, Texture &t)
{
    t.va = bo.va;
    t.cpu = nullptr;
    t.storage = {t.va, bytes, false};
    t.comptags = true;
    privateComp_[&t] = {bo, ask.compressKind};
    privateMade_.store(privateMade_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

bool Device::createCompressedTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t kind, Texture &t)
{
    tgm_layout bl;
    if (!shape(width, height, format, t) || !t.renderable || !tgm_layout_tiled(t.width, t.height, t.bpp, &bl) ||
        compressedKind(t.bpp, 0) != kind)
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    Bo bo;
    if (!takeCompressed(bl.bytes, kind, bo))
        return false;
    if (!(t.tic = allocTic())) {
        compReadyPush(bo, kind);
        return false;
    }
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
    TextureAsk ask;
    ask.width = width;
    ask.height = height;
    ask.format = format;
    ask.tiled = true;
    ask.compressKind = kind;
    noteCompressed(ask, bo, bl.bytes, t);
    writeTic(t);
    return true;
}

bool Device::createCompressedZeta(uint64_t bytes, uint32_t kind, Texture &t)
{
    Bo bo;
    if (!takeCompressed(bytes, kind, bo))
        return false;
    if (!(t.tic = allocTic())) {
        compReadyPush(bo, kind);
        return false;
    }
    t.va = bo.va;
    t.storage = {bo.va, bytes, false};
    t.comptags = true;
    compZeta_[bo.va] = bo;
    writeTic(t);
    privateMade_.store(privateMade_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return true;
}

void Device::freeCompressedZeta(uint64_t va)
{
    const auto it = compZeta_.find(va);
    if (it == compZeta_.end())
        return;
    RAW_DIAGNOSTIC(stats.compHeld.store(stats.compHeld.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed));
    RAW_DIAGNOSTIC(stats.compHeldBytes.store(stats.compHeldBytes.load(std::memory_order_relaxed) - it->second.bytes,
                                             std::memory_order_relaxed));
    k_.free(it->second);
    compZeta_.erase(it);
    comp_.freed();
}

bool Device::privateCompressed(const Texture &t)
{
    std::lock_guard<raw::Mutex> l(lock_);
    auto it = privateComp_.find(&t);
    return it != privateComp_.end() && t.comptags;
}

void Device::noteView(const Texture &of, Texture &view)
{
    if (of.comptags)
        return;
    for (auto &[texture, w] : privateWants_)
        if (of.va >= texture->va && of.va - texture->va < texture->span.bytes) {
            w.views.push_back(&view);
            return;
        }
}

void Device::forgetView(const Texture &view)
{
    for (auto &[texture, w] : privateWants_)
        w.views.erase(std::remove(w.views.begin(), w.views.end(), &view), w.views.end());
}

void Device::releasePrivateCompressed(const Texture &t)
{
    privateWants_.erase(&t);
    for (MovedPlain &m : movedPlain_)
        if (m.texture == &t)
            m.tics.clear();
    auto it = privateComp_.find(&t);
    if (it == privateComp_.end())
        return;
    PrivateComp comp = it->second;
    privateComp_.erase(it);
    // Core Animation makes its large targets again at each app open and close (boot 21: its 1792x1472 BGRA8 zoom window
    // target): the memory stays made for the next target of its kind until compTrim finds it idle
    if (comp.kind && !stopping_ && comp_.servesRefusals(comp.bo.bytes, comp.kind)) {
        compReadyPush(comp.bo, comp.kind);
        backingsChanged();
        compRearm_ = true;
        compStart();
        return;
    }
    RAW_DIAGNOSTIC(stats.compHeld--);
    RAW_DIAGNOSTIC(stats.compHeldBytes -= comp.bo.bytes);
    k_.free(comp.bo);
    comp_.freed();
}

void Device::compReadyPush(const Bo &bo, uint32_t kind)
{
    comp_.ready.push_back({bo, kind, commits(), compClock()});
    for (const auto &[texture, w] : privateWants_)
        if (w.kind == kind && compBytes(w.bytes) <= bo.bytes) {
            privateMoveDue_.store(true, std::memory_order_relaxed);
            return;
        }
}

bool Device::wantCompressed(const Texture &t, uint32_t kind, void *owner)
{
    tgm_layout bl;
    if (!kind || t.comptags || memoryOf(t) != Memory::Bl || !t.span.valid() || t.lease ||
        !tgm_layout_tiled(t.width, t.height, t.bpp, &bl) || t.pitch != bl.row_bytes || t.blockHeight != bl.y_log2)
        return false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probePlainPrivate.load(std::memory_order_relaxed) || probePlainMemory.load(std::memory_order_relaxed))
        return false;
#endif
    std::lock_guard<raw::Mutex> l(lock_);
    PrivateWant &w = privateWants_[&t];
    w.kind = kind;
    w.bytes = bl.bytes;
    w.owner = owner;
    if (comp_.fit(compBytes(w.bytes), kind) != comp_.ready.end())
        privateMoveDue_.store(true, std::memory_order_relaxed);
    return true;
}

void Device::privateMoves(void (*take)(void *owner))
{
    RAW_ENCODE_LOCK(l, lock_);
    privateMoveDue_.store(false, std::memory_order_relaxed);
    for (auto it = privateWants_.begin(); it != privateWants_.end();) {
        // one with a raw view stays plain for good (compressedFromPlain)
        if (!it->second.views.empty()) {
            it = privateWants_.erase(it);
            continue;
        }
        if (comp_.fit(compBytes(it->second.bytes), it->second.kind) != comp_.ready.end())
            take(it->second.owner);
        ++it;
    }
}

const uint8_t *Device::headerNow(uint32_t tic) const
{
    for (const MovedPlain &m : movedPlain_)
        for (size_t i = 0; i < m.tics.size(); i++)
            if (m.tics[i] == tic)
                return m.headers.cpu + 32 * i;
    return tic_.cpu + 32 * tic;
}

void Device::headersOf(uint32_t tic, std::vector<uint32_t> &tics) const
{
    tics.push_back(tic);
    for (auto [it, end] = viewTics_.equal_range(tic); it != end; ++it)
        tics.push_back(it->second.second);
    for (auto [it, end] = attachmentViewTics_.equal_range(tic); it != end; ++it)
        tics.push_back(it->second.second);
    for (auto [it, end] = partTics_.equal_range(tic); it != end; ++it)
        headersOf(it->second.second, tics);
}

// The headers are rewritten by the move's job, after its copy: work submitted before it reads the plain memory through
// them, and a gpuResourceID or argument buffer naming one keeps its index and reads the compressed memory from that job on
bool Device::compressedFromPlain(Texture &t)
{
    Bo bo;
    uint32_t kind = 0;
    uint64_t bytes = 0;
    std::vector<uint32_t> tics;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        const auto want = privateWants_.find(&t);
        if (want == privateWants_.end())
            return false;
        // a raw view may go before the move's job runs, and its header index be another texture's by then
        if (!want->second.views.empty()) {
            privateWants_.erase(want);
            return false;
        }
        if (!takeCompressed(want->second.bytes, want->second.kind, bo))
            return false;
        kind = want->second.kind;
        bytes = want->second.bytes;
        headersOf(t.tic, tics);
    }
    Buffer headers;
    const bool named = createBuffer(32 * tics.size(), headers, true);
    for (size_t i = 0; named && i < tics.size(); i++) {
        uint32_t w[8];
        memcpy(w, tic_.cpu + 32 * tics[i], 32);
        ticAt(w, ticAddress(w) - t.va + bo.va);
        memcpy(headers.cpu + 32 * i, w, 32);
    }
    Texture comp = t;
    comp.va = bo.va;
    comp.cpu = nullptr;
    comp.span = {};
    comp.storage = {bo.va, bytes, false};
    comp.comptags = true;
    uint32_t seq = 0;
    CommandBuffer *rc = named ? begin() : nullptr;
    bool copied = rc != nullptr;
    if (copied) {
        rc->use(headers, false);
        rc->useAddress(tic_.va, tic_.bytes, true);
        BlitEncoder *b = rc->blit();
        copyImages(*b, t, comp);
        for (size_t i = 0; i < tics.size(); i++)
            b->copyBytes(headers.va + 32 * i, tic_.va + 32ull * tics[i], 32);
        b->end();
        copied = rc->commit(seq);
        if (!copied)
            rc->abandon();
    }
    std::lock_guard<raw::Mutex> l(lock_);
    std::vector<Texture *> views;
    if (const auto want = privateWants_.find(&t); want != privateWants_.end()) {
        views = std::move(want->second.views);
        privateWants_.erase(want);
    }
    if (!copied) {
        compReadyPush(bo, kind);
        if (named)
            freeSpan(headers.span, headers.wc ? Memory::Wc : Memory::Pool, headers.lease);
        os_log_error(OS_LOG_DEFAULT, "raw: a plain %ux%u texture of format %u could not move to compressed memory",
                     t.width, t.height, t.format);
        return false;
    }
    movedPlain_.push_back({&t, t.span, headers, std::move(tics), seq, channelGeneration()});
    const uint64_t from = t.va;
    t.va = bo.va;
    t.cpu = nullptr;
    t.span = {};
    t.storage = comp.storage;
    t.comptags = true;
    privateComp_[&t] = {bo, kind};
    // made on another thread while the job was built, so no work names them yet: their headers change here
    for (Texture *v : views) {
        ticAt(reinterpret_cast<uint32_t *>(tic_.cpu + 32 * v->tic), v->va - from + t.va);
        v->va = v->va - from + t.va;
        v->cpu = nullptr;
        v->storage = t.storageRange();
        v->comptags = true;
    }
    RAW_DIAGNOSTIC(stats.privateCompressedMoves++);
#if SWITCH_RAW_DIAGNOSTICS
    os_log(OS_LOG_DEFAULT, "raw: private %ux%u f%u moved to compressed memory kind 0x%x (%llu KiB)", t.width, t.height,
           t.format, kind, (unsigned long long)(bo.bytes >> 10));
#endif
    return true;
}

void Device::freeMovedPlain()
{
    RAW_ENCODE_LOCK(l, lock_);
    for (auto it = movedPlain_.begin(); it != movedPlain_.end();) {
        const bool lost = it->generation != channelGeneration();
        if (!lost && !done(it->seqno)) {
            ++it;
            continue;
        }
        // its job was on a channel since lost, which may have stopped before it: none of that channel's work runs now
        if (lost)
            for (size_t i = 0; i < it->tics.size(); i++)
                memcpy(tic_.cpu + 32 * it->tics[i], it->headers.cpu + 32 * i, 32);
        Lease *none = nullptr;
        freeSpan(it->span, Memory::Bl, none);
        freeSpan(it->headers.span, it->headers.wc ? Memory::Wc : Memory::Pool, it->headers.lease);
        it = movedPlain_.erase(it);
    }
}

}  // namespace raw
