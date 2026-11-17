// SPDX-License-Identifier: MIT
// See raw_gpu.h (CommandBuffer::render and the backings of its attachments).
#include "raw_lock.h"
#include "../fx/fx_image.h"
#include "raw_attachment_lifetime.h"
#include "raw_gpu.h"
#include "raw_layers.h"
#include "raw_rotated_pass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "raw_gpu_internal.h"

namespace raw {

// A memoryless attachment's memory: one per size, format, tag and layout, kept while passes use it (m2v 0105 gives a memoryless
// attachment linear memory in a pass that renders to linear memory; NVK renders a pass with a block-linear colour target or a
// depth/stencil attachment into block-linear colour). Called with the device lock held.
Device::PlainMem *Device::largestPlain(uint32_t tag, uint32_t memory, bool layered) const
{
    PlainMem *best = nullptr;
    for (const auto &m : plainMems_)
        if (m->holds(tag, memory, layered) && (!best || m->span.bytes > best->span.bytes))
            best = m.get();
    return best;
}

Device::Backing *Device::plainBacking(const Texture &m, uint32_t tag, bool blockLinear)
{
    Texture t;
    fx::MipChain c;
    uint64_t bytes = 0, align = kBlAlign;
    uint32_t memory = kPlainPool;
    const FormatInfo *f = formatInfo(m.format);
    if (m.zeta) {
        memory = kPlainZeta + zetaFormatIndex(m.format);
        uint64_t zetaAlign = 0;
        if (!zetaShape(m.width, m.height, m.format, m.samples, t, bytes, zetaAlign))
            return nullptr;
        align = std::max(align, zetaAlign);
    } else if (blockLinear) {
        memory = kPlainBl;
        if (!f || !shape(m.width, m.height, m.format, t) || !fx::msLayout(m.width, m.height, m.samples, t.bpp, c))
            return nullptr;
        bytes = c.bytes;
        align = std::max<uint64_t>(align, c.align);
        t.pitch = c.level[0].rowBytes;
        t.blockLinear = true;
        t.blockHeight = c.level[0].yLog2;
        t.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
        t.samples = m.samples;
    } else if (m.layers > 1) {
        return nullptr;
    } else {
        if (!f || !shape(m.width, m.height, m.format, t))
            return nullptr;
        t.pitch = (m.width * t.bpp + 255) & ~255u;
        bytes = uint64_t(t.pitch) * t.height;
    }
    // a layered pass's: its layers one layer's whole tiles apart (NIL's array stride of a one-level image)
    t.layers = std::max(m.layers, 1u);
    const bool layered = t.layers > 1;
    if (layered)
        bytes = (t.layerBytes * t.layers + align - 1) / align * align;
    PlainMem *mem = largestPlain(tag, memory, layered);
    if (!mem || mem->span.bytes < bytes || (mem->span.va & (align - 1))) {
        auto made = std::make_unique<PlainMem>();
        made->tag = tag;
        made->memory = memory;
        made->layered = layered;
        made->align = align;
        const uint64_t want = sizeClassBytes(bytes, kBlAlign);
        const bool ok = memory == kPlainPool ? poolAlloc(want, made->span)
                        : memory == kPlainBl
                            ? blAlloc(want, align, made->span)
                            : tiledAlloc(zChunks_[memory - kPlainZeta], kZetaFormatInfo[memory - kPlainZeta].kind, want,
                                         align, made->span);
        if (!ok || (made->span.va & (align - 1))) {
            if (ok)
                freePlainSpan(*made);
            return nullptr;
        }
        RAW_DIAGNOSTIC(stats.backingBytes += made->span.bytes);
        RAW_DIAGNOSTIC(glassMemEvent("memoryless plain memory made", made->span.bytes));
        plainMems_.push_back(std::move(made));
        mem = plainMems_.back().get();
        // the views of its tag's smaller memory: no pass takes them again (an animated layer seldom asks a size twice), so it
        // goes back once their last uses complete rather than 10 s on
        for (Backing *b : backings_)
            if (b->plain && b->plain != mem && b->plain->holds(tag, memory, layered))
                b->retired = true;
        backingsChanged();
        compRearm_ = true;  // an idle process gives it back when it turns stale
        compStart();
    }
    t.va = mem->span.va;
    t.storage = {t.va, bytes, false};
    t.cpu = memory == kPlainPool ? mem->span.cpu : nullptr;
    if (m.zeta) {  // a depth resolve's or a turn copy's source samples it
        if (!(t.tic = allocTic())) {
            if (!mem->views)
                dropPlain(mem);
            return nullptr;
        }
        writeTic(t);
    }
    if (!m.zeta) {
        if (!(t.tic = allocTic())) {
            if (!mem->views)
                dropPlain(mem);
            return nullptr;
        }
        if (blockLinear) {
            uint32_t w[8];
            fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, 1, w);
            memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
        } else {
            t.renderable = t.renderable && linearTarget(t);
            writeTic(t);
        }
    }
    auto *b = new Backing;
    b->texture = t;
    b->tag = tag;
    b->plain = mem;
    b->usedNs = compClock();
    mem->views++;
    RAW_DIAGNOSTIC(stats.backings++);
    return b;
}

void Device::freePlainSpan(PlainMem &mem)
{
    if (mem.memory == kPlainPool)
        poolFree(mem.span);
    else if (mem.memory == kPlainBl)
        blFree(mem.span);
    else
        tiledFree(zChunks_[mem.memory - kPlainZeta], mem.span);
}

void Device::dropPlain(PlainMem *mem)
{
    RAW_DIAGNOSTIC(stats.backingBytes -= mem->span.bytes);
    RAW_DIAGNOSTIC(glassMemEvent("memoryless plain memory freed", mem->span.bytes));
    freePlainSpan(*mem);
    plainMems_.erase(std::find_if(plainMems_.begin(), plainMems_.end(), [&](const std::unique_ptr<PlainMem> &p) {
        return p.get() == mem;
    }));
}

void Device::plainFree(Backing &b)
{
    if (b.texture.tic)
        freeTic(b.texture.tic);
    RAW_DIAGNOSTIC(stats.backings--);
    PlainMem *mem = b.plain;
    b.plain = nullptr;
    if (!--mem->views)
        dropPlain(mem);
}

Texture *Device::lastBacking(const Texture &m, uint32_t tag, bool blockLinear, bool compression, bool copied)
{
    const uint64_t epoch = backingEpoch_.load(std::memory_order_seq_cst);
    for (const BackingAsk &ask : asks_) {
        if (ask.epoch != epoch || !ask.same(m, tag, blockLinear, compression, copied, kindProbes()))
            continue;
        Backing *b = ask.backing;
        b->lastCommit.store(commitCount_, std::memory_order_relaxed);
        b->lastClosed.store(closedCount_, std::memory_order_seq_cst);
        // a trim moves the epoch before it reads the stamps: one that began since reads ours or is seen here
        return backingEpoch_.load(std::memory_order_seq_cst) == epoch ? &b->texture : nullptr;
    }
    return nullptr;
}

Texture *Device::backing(const Texture &m, uint32_t tag, bool blockLinear, bool compression, bool copied)
{
    BackingAsk ask{m.width,     m.height,    m.format, tag,         std::max(m.layers, 1u), m.samples, m.zeta,
                   blockLinear, compression, copied,   kindProbes()};
    blockLinear |= m.zeta;
    // a layered pass's backing: plain memory of its layers (compressed backings are one layer's)
    const uint32_t kind = compression && m.layers <= 1 ? compKind(m, blockLinear, copied) : 0;
    const CompMem *mem = kind ? comp_.largest(tag, kind) : nullptr;
    Backing *plain = nullptr;
    blockLinear |= m.samples > 1;
    const auto asked = [&](Backing *b) {
        b->lastCommit.store(commitCount_, std::memory_order_relaxed);
        b->lastClosed.store(closedCount_, std::memory_order_relaxed);
        if (tag < lastBackings_.size())
            lastBackings_[tag] = b;
        ask.backing = b;
        ask.epoch = backingEpoch_.load(std::memory_order_relaxed);
        asks_[nextAsk_++ % asks_.size()] = ask;
        return &b->texture;
    };
    const auto matches = [&](const Backing *b) {
        return b && !b->retired && b->texture.width == m.width && b->texture.height == m.height &&
               b->texture.format == m.format && b->tag == tag && b->texture.blockLinear == blockLinear &&
               b->texture.samples == m.samples && b->texture.layers == std::max(m.layers, 1u);
    };
    if (Backing *last = tag < lastBackings_.size() ? lastBackings_[tag] : nullptr;
        matches(last) && backingCompressionMatches(last->comp != nullptr, last->comp, mem, kind != 0))
        return asked(last);
    for (Backing *b : backings_)
        if (matches(b)) {
            if (b->plain && b->plain != largestPlain(tag, b->plain->memory, b->plain->layered)) {
                b->retired = true;  // larger plain memory of its tag took its place: a view of that replaces it
                backingsChanged();
                continue;
            }
            if (backingCompressionMatches(b->comp != nullptr, b->comp, mem, kind != 0))
                return asked(b);
            if (b->comp && kind) {
                b->retired = true;  // larger memory of its tag and kind took its place: a view of that replaces it
                backingsChanged();
            } else if (!b->comp)
                plain = b;
        }
    if (Backing *c = kind ? compBacking(m, tag, kind) : nullptr) {
        backings_.push_back(c);
        return asked(c);
    }
    if (plain)
        return asked(plain);
    Backing *view = plainBacking(m, tag, blockLinear);
    if (!view)
        return nullptr;
    backings_.push_back(view);
    return asked(view);
}

// Backings no pass used for 600 command buffers (~10 s of frames) and whose last use completed go back to the pool; a retired
// one (a compressed backing, or one on larger memory of its tag, replaced it) as soon as its last use completed, as no pass
// takes it again; one on compressed or shared plain memory after 10 s unused too, also while the process commits nothing
// (raw_comp.h backingStale; the compressed-memory thread trims then).
void Device::trimBackings()
{
    // the counts before the backings' stamps: a commit stores its stamps before it moves them
    const uint64_t committed = commits(), closedNow = closedCount_.load(std::memory_order_acquire);
    const uint64_t now = compClock();
    const auto stale = [&](const Backing *b) {
        return backingStale(committed, b->lastCommit.load(std::memory_order_seq_cst), closedNow,
                            b->lastClosed.load(std::memory_order_seq_cst), now,
                            b->usedNs.load(std::memory_order_relaxed), b->retired, b->comp || b->plain) &&
               (!b->submitted.load(std::memory_order_relaxed) || done(b->lastUse.load(std::memory_order_relaxed)));
    };
    const size_t ready = comp_.ready.size();
    compTrim(now);
    for (auto it = spares_.begin(); it != spares_.end();) {
        if (committed - it->freedAt > 600) {
            freeChunk(chunks_[it->chunk]);
            it = spares_.erase(it);
            continue;
        }
        ++it;
    }
    // A pass's stamps (lastBacking, with no lock) only make a backing less stale: where none is stale before the epoch
    // moves none is after, and the passes' kept asks stay good
    if (comp_.ready.size() != ready || std::any_of(backings_.begin(), backings_.end(), stale)) {
        backingsChanged();
        for (auto it = backings_.begin(); it != backings_.end();) {
            Backing *b = *it;
            if (!stale(b)) {
                ++it;
                continue;
            }
            if (b->tag < lastBackings_.size() && lastBackings_[b->tag] == b)
                lastBackings_[b->tag] = nullptr;
            if (b->comp) {
                compFree(*b);
            } else if (b->plain) {
                plainFree(*b);
            }
            trimmedBackings_.push_back(b);
            trimmedPending_.store(true, std::memory_order_release);
            it = backings_.erase(it);
        }
    }
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t retired = 0;
    for (const Backing *b : backings_)
        retired += !b->comp && b->retired;
    stats.retiredBackings = retired;
    uint64_t tiledFree = tiledFreeBytes(blChunks_);
    for (const std::vector<Chunk> &z : zChunks_)
        tiledFree += tiledFreeBytes(z);
    stats.tiledFreeBytes = tiledFree;
#endif
}

RenderEncoder *CommandBuffer::render(const PassDesc &pass, const char **why)
{
    const auto refused = [why](const char *rule) -> RenderEncoder * {
        if (why)
            *why = rule;
        return nullptr;
    };
    constexpr const char *kNoMemory =
        "no memory for a pass's attachment backing: MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)";
    RAW_ENCODE_SPAN(PassBegin);
    RenderEncoder &e = render_;
    // a pass's lists keep the last one's capacity: no allocation a steady frame
    std::vector<RenderEncoder::Body> bodies = std::move(e.bodies_);
    std::vector<std::array<uint32_t, 4>> rects = std::move(e.rects_);
    SplitPassPlan splitPlan = std::move(e.splitPlan_);
    std::vector<RenderEncoder::Clear> clears = std::move(e.initialClears_);
    std::vector<bool> tiles = std::move(e.tiles_);
    std::vector<GeometryAnalysis> written = e.written_.release();
    bodies.clear();
    rects.clear();
    splitPlan.clear();
    clears.clear();
    // made again where it is, not built aside and moved in (the encoder is 7 KB)
    e.~RenderEncoder();
    new (&e) RenderEncoder();
    e.bodies_ = std::move(bodies);
    e.rects_ = std::move(rects);
    e.splitPlan_ = std::move(splitPlan);
    e.initialClears_ = std::move(clears);
    e.tiles_ = std::move(tiles);
    e.written_.adopt(std::move(written));
    e.cb_ = this;
    e.dev_ = dev_;
    e.pass_ = pass;
    RAW_DIAGNOSTIC(e.prologueMark_ = dev_->spans_.reserve());
    // the render hardware takes one memory layout for all of a pass's color targets (nvk_rendering_linear): block-linear when
    // one of them is, or is a pitch target the hardware cannot take (rendered through a shadow either way), or beside a zeta,
    // its pitch ones then rendered through block-linear shadows as NVK does, or when every one is memoryless; memoryless ones
    // get a backing of the pass's layout
    const Texture *zeta = pass.depthStencil.texture;
    if (zeta && !zeta->zeta)
        return refused("a depth/stencil attachment of a colour pixel format, which Metal's validation layer stops");
    // every attachment of one sample count (Metal's rule)
    uint8_t samples = zeta ? zeta->samples : 0;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture) {
            if (samples && t->samples != samples)
                return refused("attachments of different sample counts, which Metal's validation layer stops "
                               "(MTLRenderPipeline.h rasterSampleCount: the samples per fragment of the render pass)");
            samples = t->samples;
        }
    e.samples_ = std::max<uint8_t>(samples, 1);
    if (const RotatedRefusal turn = passRotation(pass, e.rot_); turn != RotatedRefusal::None)
        return refused(rotatedRefusalText(turn));
    if (const char *layered = layeredRefusal(pass, e.samples_, e.rot_.turned()))
        return refused(layered);
    const uint32_t layers = e.pass_.layers = std::max(pass.layers, 1u);
    const auto layered = [layers](Texture shape) {
        shape.layers = layers;
        return shape;
    };
    // a turned pass's attachment that is not turned is transient (passRotation): a backing laid out as the plane stands in
    const auto backed = [&](const Texture &t) {
        return t.memoryless || standsInTurned(t, e.rot_);
    };
    const bool hidden = !zeta && pass.hiddenZeta;
    bool blockLinear = zeta != nullptr || hidden || layers > 1, pitchTarget = false;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture; t && !backed(*t)) {
            if (t->blockLinear || t->shadowTarget || (pass.arrayedStorageMask >> j & 1))
                blockLinear = true;
            else
                pitchTarget = true;
        }
    blockLinear |= !pitchTarget;
    // the device's backings: those the last passes asked the same of with no lock, else under the device's lock once a
    // pass however many attachments need one (the compression thread trims them under it)
    std::unique_lock<raw::Mutex> backingLock(dev_->lock_, std::defer_lock);
    const auto backing = [&](const Texture &shape, uint32_t tag, bool linear, bool compression, bool copied = false) {
        if (Texture *b = backingLock.owns_lock() ? nullptr : dev_->lastBacking(shape, tag, linear, compression, copied))
            return b;
        if (!backingLock.owns_lock()) {
            backingLock.lock();
            RAW_ENCODE_COUNT(Locks);
        }
        return dev_->backing(shape, tag, linear, compression, copied);
    };
    if (zeta && backed(*zeta)) {
        Texture *b = backing(layered(planeShaped(*zeta, e.rot_)), 0, false, true);
        if (!b)
            return refused(kNoMemory);
        e.pass_.depthStencil.texture = b;
        backings_.push_back(b);
    }
    uint8_t scratchMask = 0;
    for (uint32_t j = 0; j < pass.count && j < 8; ++j)
        if (const Texture *t = pass.color[j].texture;
            t && (backed(*t) || t->shadowTarget || t->zetaPages || (blockLinear && !t->blockLinear)))
            scratchMask |= uint8_t(1u << j);
    const auto scratchTags = transientColorTags(scratchMask);
    for (uint32_t j = 0; j < pass.count && j < 8; j++) {
        const Texture *t = pass.color[j].texture;
        if (!t)
            continue;
        if (!t->renderable && !t->shadowTarget)
            return refused("a colour attachment the raw path does not render to (pass.fmt's at record)");
        const bool mixed = blockLinear && !t->blockLinear && !backed(*t);
        if (((t->shadowTarget || t->zetaPages) && !backed(*t)) ||
            mixed) {  // rendered through a pool image (end(): copied in before, back after)
            RAW_DIAGNOSTIC(dev_->stats.xrPlaneShadows += t->xrPlane != 0);
            if (mixed) {
                RAW_DIAGNOSTIC(dev_->stats.rtMixed++);
            } else if (t->shadowTarget) {
                RAW_DIAGNOSTIC(dev_->stats.rtUnaligned++);
                RAW_DIAGNOSTIC(dev_->stats.rtUnalignedPitch = t->pitch);
                RAW_DIAGNOSTIC(dev_->stats.rtUnalignedVa = uint32_t(t->va & 255));
            }
            // a zeta's view takes plain memory: gpu.x1.ce_compressed proved the copy engine over compressed BGRA8 and
            // RGBA16Float texels, not over a zeta's depth bits and stencil
            Texture *b = backing(*t, scratchTags[j], blockLinear,
                                 !t->zetaPages && (dev_->storageCompressedOk() || !(pass.storageMask >> j & 1)), true);
            if (!b || !b->renderable)
                return refused(kNoMemory);
            e.pass_.color[j].texture = b;
            e.shadowOf_[j] = t;
            backings_.push_back(b);
            continue;
        }
        if (backed(*t)) {  // back it for this pass
            Texture *b = backing(layered(planeShaped(*t, e.rot_)), scratchTags[j], blockLinear,
                                 layers == 1 && (dev_->storageCompressedOk() || !(pass.storageMask >> j & 1)));
            if (!b)
                return refused(kNoMemory);
            e.pass_.color[j].texture = b;
            e.memoryless_[j] = true;
            backings_.push_back(b);
        }
    }
    if (hidden) {
        Texture *b = backing(e.hiddenZetaShape(pass), 0, false, true);
        if (!b)
            return refused(kNoMemory);
        e.pass_.depthStencil = {b, true, true, 1.0f, 0};
        e.hiddenZeta_ = true;
        backings_.push_back(b);
    }
    if (const Texture *stencil = pass.separate.stencil) {
        constexpr uint32_t kRG8Unorm = 30;
        Texture shape;
        shape.width = stencil->width;
        shape.height = stencil->height;
        shape.samples = stencil->samples;
        shape.layers = layers;
        shape.format = kZetaFormat;
        shape.zeta = shape.memoryless = true;
        // a Depth16Unorm's pair names no depth here: the caller hands the pass a converted scratch (depthConvert)
        Texture *b = pass.separate.depth ? backing(shape, Device::kSeparateZetaTag, false, true, true) : nullptr;
        shape.format = kRG8Unorm;
        shape.zeta = false;
        Texture *temporary =
            b || !pass.separate.depth ? backing(shape, Device::kSeparateTempTag, true, false, true) : nullptr;
        if (!temporary)
            return refused(kNoMemory);
        if (b) {
            e.pass_.depthStencil.texture = b;
            backings_.push_back(b);
        }
        e.separate_ = pass.separate;
        e.separateTemp_ = temporary;
        backings_.push_back(temporary);
    }
    if (backingLock.owns_lock())
        backingLock.unlock();
    // the depth's stored region (raw_zcull_planes.h), taken before this pass's own write leaves it stale
    const ZcullStored zcullHeld = zeta ? zcullPlanes_.held(zeta->va) : ZcullStored();
    for (uint32_t j = 0; j < e.pass_.count && j < 8; ++j)
        if (const Texture *texture = e.pass_.color[j].texture)
            use(*texture, true);
    if (const Texture *texture = e.pass_.depthStencil.texture)
        use(*texture, true);
    // the render area: from the origin, the smallest attachment's extent, each axis within the pass's target size where it
    // names one (m2v render_pass.rs `area`); Metal's default viewport and scissor cover it (render_state.rs `metal_defaults`)
    const Texture *extents[9] = {zeta};
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        extents[1 + j] = e.pass_.color[j].texture;
    for (const Texture *t : extents)
        if (t) {
            e.areaW_ = e.areaW_ ? std::min(e.areaW_, t->width) : t->width;
            e.areaH_ = e.areaH_ ? std::min(e.areaH_, t->height) : t->height;
        }
    if (e.rot_.turned())
        frameArea(pass, e.rot_, e.areaW_, e.areaH_);
    if (pass.width)
        e.areaW_ = std::min(e.areaW_, pass.width);
    if (pass.height)
        e.areaH_ = std::min(e.areaH_, pass.height);
    // (a separate depth and stencil pair renders a scratch zeta of the pass's, which keeps zcull off)
    ZcullAsk zask;
    zask.on = dev_->zcullOn_;
    zask.depth = zeta && !pass.separate.stencil && zetaHasDepth(zeta->format);
    zask.cleared = pass.depthStencil.clearDepth && !std::isnan(pass.depthStencil.depth);
    zask.layers = layers;
    zask.zetaLayers = zeta ? std::max(zeta->layers, 1u) : 1;
    zask.atStorageBase = zeta && (!zeta->storage.va || zeta->va == zeta->storage.va);
    const Rect zcullArea = e.planeArea();
    const bool zcullKeep =
        zeta && e.pass_.depthStencil.texture == zeta && pass.depthStencil.stored && pass.depthStencil.loadedLater;
    zcullBegin(e.zcull_, zask, dev_->zcullInfo_, zcullArea.x0, zcullArea.y0, zcullArea.x1 - zcullArea.x0,
               zcullArea.y1 - zcullArea.y0, layers, zcullHeld, zcullKeep, [this](uint64_t bytes) {
                   uint64_t va = 0;
                   dev_->alloc(dev_->upload_, uint32_t(bytes), kZcullStorageAlign, va);
                   return va;
               });
    // DontCare can become a clear after later draws establish readBits_. Treat its deferred prologue write like Clear.
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (e.pass_.color[j].texture && e.pass_.color[j].load != ColorAttachment::Load)
            e.attachmentWrites_.rop |= uint8_t(1u << j);
    if (e.attachmentWrites_.rop) {
        DrawGeometry cleared;
        cleared.limit = {0, 0, e.areaW_, e.areaH_};
        e.written_.add(cleared);
    }
    // the body goes straight into the push ring; the pass prologue (targets, clears) is written at end() and runs first
    e.reserve();
    e.setViewport(0, 0, e.areaW_, e.areaH_, 0, 1);
    e.setScissor(0, 0, e.areaW_, e.areaH_);
    e.extrasAtBegin_ = extrasLive_;
    if (extrasLive_) {
        heldSegment_.lose();
        RAW_DIAGNOSTIC(dev_->stats.segmentExtrasLosses++);
    } else {
        heldSegment_.passBoundary();
    }
    extrasLive_ = nullptr;
    // a target the pass does not load starts over: no earlier rung-2 blend is under what it draws (rule fetch.blend)
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (pass.color[j].texture && pass.color[j].load != ColorAttachment::Load)
            for (const Texture *t : {static_cast<const Texture *>(pass.color[j].texture), e.shadowOf_[j]})
                if (t)
                    __atomic_store_n(&t->ropBlendSerial, uint64_t(0), __ATOMIC_RELAXED);
    passes++;
    return &e;
}

}  // namespace raw
