// SPDX-License-Identifier: MIT
// See raw_gpu.h. The copies follow NVK's nouveau_copy_rect and nvk_cmd_copy_memory_ce (MIT).
#include "raw_lock.h"
#include "../fx/fx_image.h"
#include "raw_gpu.h"
#include "raw_kit.h"
#include "raw_zeta_cpu.h"
#include "raw_srgb_pair.h"
#include "raw_xr_parts.h"
#include "../fx/fx_mip_route.h"
#include "raw_tiled_cache.h"

#include <algorithm>
#include <cstdio>

#include "raw_gpu_internal.h"

namespace raw {

LoopStream BlitEncoder::stream()
{
    return cb_->loopStream(p_, start_);
}

BlitEncoder *CommandBuffer::blit()
{
    BlitEncoder &e = blit_;
    e = BlitEncoder();
    heldSegment_.lose();
    e.cb_ = this;
    e.dev_ = dev_;
    uint64_t va;
    e.start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    e.p_ = Push{e.start_, e.start_ + (1 << 12)};
    loops_.openSegment(e.stream());
    // what the render work before wrote is complete before the copy engine reads it (NVK: a WFI on the 3D subchannel; the
    // switch to the copy subchannel then waits for the 3D engine anyway)
    const BarrierForms forms = barrierForms();
    [[maybe_unused]] const Opened opened =
        blitOpening(e.p_, switchFormFor(forms.switchForm, lastEngine_, BarrierEngine::Copy), entryFormFor(forms.entry),
                    lastEngine_);
    e.before_ = lastEngine_;
    e.opened_ = e.p_.at;
    lastEngine_ = BarrierEngine::Copy;
    RAW_DIAGNOSTIC(taken_.count(opened));
    blits++;
    return &e;
}

static uint64_t rowStorageBytes(uint32_t pitch, uint32_t width, uint32_t height, uint32_t element)
{
    if (!width || !height || !element)
        return 0;
    const uint64_t rows = uint64_t(pitch) * (height - 1), last = uint64_t(width) * element;
    return last <= UINT64_MAX - rows ? rows + last : 0;
}

namespace {
// SET_REMAP_COMPONENTS of one-, two- or four-byte components (`size`): DST_X..W each SRC_X..W (0..3) or NO_WRITE (6), the
// source's and destination's component counts (clb0b5)
constexpr uint32_t kRemapNoWrite = NVB0B5_SET_REMAP_COMPONENTS_DST_X_NO_WRITE;

constexpr uint32_t remapOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t src, uint32_t dst)
{
    return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (src - 1) << 20 | (dst - 1) << 24;
}

// `lines` rows of `width` texels of `bpp` bytes, pitch to pitch
void copyRows(Push &p, uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width, uint32_t lines,
              uint32_t bpp, Device::Stats &stats)
{
    copySides(p, {src, srcPitch}, {dst, dstPitch}, width, lines, bpp, stats);
}
}  // namespace

TextureTransferResult Device::transferTexture(const Texture &texture, uint32_t level, uint32_t slice,
                                              const uint32_t origin[3], const uint32_t size[3], void *bytes,
                                              uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner)
{
    Texture view;
    const Texture *selected = &texture;
    if (texture.chain && level) {  // a chain's level 0 is its own image
        if (!chainLevelImage(texture, level, view))
            return TextureTransferResult::Unsupported;
        selected = &view;
    } else if (level) {
        return TextureTransferResult::Unsupported;
    }
    const Texture &t = *selected;
    ZetaCpuCopy zeta;
    fx::MipChain grid;
    if (!t.va || (texture.zeta && !zetaCpuCopy(texture.format, zeta)) ||
        (t.samples > 1 && (!texture.zeta || !fx::msLayout(t.width, t.height, t.samples, t.bpp, grid))) ||
        t.converted() || t.blockW > 1 || t.blockH > 1 || xrForm(t.format) != XrForm::None || !size[0] || !size[1] ||
        !size[2] || origin[0] > t.width || size[0] > t.width - origin[0] || origin[1] > t.height ||
        size[1] > t.height - origin[1])
        return TextureTransferResult::Unsupported;
    const bool volume = t.type == fx::kTexThreeD;
    if (volume ? (slice || origin[2] > t.depth || size[2] > t.depth - origin[2])
               : (slice >= std::max(1u, t.layers) || origin[2] || size[2] != 1))
        return TextureTransferResult::Unsupported;
    TextureTransfer region;
    region.texture = sideOf(t, origin[0], origin[1], volume ? origin[2] : slice);
    if (t.samples > 1) {  // the pixels' cells of the sample grid
        region.texture = {
            t.va, t.pitch,      origin[0] << grid.sxLog2, origin[1] << grid.syLog2, t.height << grid.syLog2,
            true, t.blockHeight};
        region.samples = t.samples;
        region.sxLog2 = grid.sxLog2;
        region.syLog2 = grid.syLog2;
    }
    region.width = size[0];
    region.height = size[1];
    region.planes = size[2];
    region.bpp = texture.zeta ? zeta.bytes : t.bpp;
    region.remapIn = zeta.remapIn;
    region.remapOut = zeta.remapOut;
    if (!transfers_.copy(k_, set_, lock_, region, bytes, row, image, write, std::move(owner)))
        return TextureTransferResult::Failed;
    RAW_DIAGNOSTIC((write ? stats.textureUploads : stats.textureReadbacks).fetch_add(1, std::memory_order_relaxed));
#if SWITCH_RAW_DIAGNOSTICS
    if (write && glassMemOn.load(std::memory_order_relaxed))
        glassMem->wrote(texture.storageRange().va ? texture.storageRange().va : texture.va,
                        commitCount_.load(std::memory_order_relaxed), stats.memEvents.load(std::memory_order_relaxed));
#endif
    return TextureTransferResult::Complete;
}

bool Device::copyCallerPages(const Texture &t, std::shared_ptr<void> owner)
{
    if (!t.callerPages)
        return true;
    if (!transfers_.copyBytes(k_, set_, lock_, t.callerPages->bo.va, t.imagePitchVa, t.storage.bytes, std::move(owner)))
        return false;
    RAW_DIAGNOSTIC(stats.textureUploads.fetch_add(1, std::memory_order_relaxed));
    return true;
}

// ---- blit encoder ----

// NVK's copies of pitch memory: a byte copy (vkCmdCopyBuffer, nvk_cmd_copy_memory_ce: 128 KiB per launch, no remap) or a
// rectangle of pitch rows (vkCmdCopyImage of linear images, nouveau_copy_rect: remap of min(bpp, 4)-byte components, so the line
// length counts pixels). LAUNCH_DMA: DATA_TRANSFER_TYPE PIPELINED, FLUSH_ENABLE, SRC/DST_MEMORY_LAYOUT PITCH, MULTI_LINE_ENABLE.

void BlitEncoder::room(uint32_t words)
{
    if (p_.room() >= words + 8)
        return;
    checkRoom(p_, "a blit encoder");
    cb_->loops_.closeSegment(stream());  // a new reservation: the copies so far become a segment
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    p_ = Push{start_, start_ + (1 << 12)};
    cb_->loops_.openSegment(stream());
}

void BlitEncoder::copyBytes(uint64_t src, uint64_t dst, uint64_t bytes)
{
    cb_->bufferWrites_.note(dst, bytes);
    for (uint64_t done = 0; done < bytes;) {
        room();
        const uint32_t n = uint32_t(std::min<uint64_t>(bytes - done, 1u << 17));
        p_.inc(CE_OFFSET_IN_UPPER,
               {uint32_t((src + done) >> 32), uint32_t(src + done), uint32_t((dst + done) >> 32), uint32_t(dst + done)},
               SUBC_COPY);
        p_.inc(CE_LINE_LENGTH_IN, {n, 1}, SUBC_COPY);
        p_.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);
        RAW_DIAGNOSTIC(dev_->stats.ceOps++);
        RAW_DIAGNOSTIC(dev_->stats.ceBytes += n);
        done += n;
    }
}

// SET_REMAP_COMPONENTS: DST_X..W CONST_A (4), COMPONENT_SIZE ONE (0) or FOUR (3) bytes, one source and destination
// component; LAUNCH_DMA: PIPELINED, FLUSH, pitch both sides, REMAP
namespace {
constexpr uint32_t kFillConstA = 0x4444, kFillMaxDim = 1u << 15;
constexpr uint32_t kFillLaunch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 10;
}  // namespace

void BlitEncoder::fillRun(uint64_t at, uint32_t width, uint32_t height, [[maybe_unused]] uint32_t unit)
{
    room();
    p_.inc(CE_OFFSET_IN_UPPER + 8, {uint32_t(at >> 32), uint32_t(at)}, SUBC_COPY);
    p_.inc(CE_LINE_LENGTH_IN, {width, height}, SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kFillLaunch | (height > 1 ? 1u << 9 : 0), SUBC_COPY);
    RAW_DIAGNOSTIC(dev_->stats.ceOps++);
    RAW_DIAGNOSTIC(dev_->stats.ceBytes += uint64_t(width) * height * unit);
}

void BlitEncoder::fillWords(uint64_t dst, uint64_t bytes, uint32_t word)
{
    cb_->bufferWrites_.note(dst, bytes);
    if (bytes < 4)
        return;
    room();
    p_.m(CE_SET_REMAP_CONST_A, word, SUBC_COPY);
    p_.m(CE_SET_REMAP_COMPONENTS, kFillConstA | 3u << 16, SUBC_COPY);
    p_.inc(CE_PITCH_IN, {kFillMaxDim * 4, kFillMaxDim * 4}, SUBC_COPY);
    for (uint64_t at = dst, left = bytes & ~uint64_t(3); left >= 4;) {
        const uint64_t words = left / 4;
        const uint32_t width = uint32_t(std::min<uint64_t>(words, kFillMaxDim)),
                       height =
                           words >= kFillMaxDim ? uint32_t(std::min<uint64_t>(words / kFillMaxDim, kFillMaxDim)) : 1;
        fillRun(at, width, height, 4);
        at += uint64_t(width) * height * 4;
        left -= uint64_t(width) * height * 4;
    }
}

void BlitEncoder::fillBytes(uint64_t dst, uint64_t bytes, uint8_t value)
{
    cb_->bufferWrites_.note(dst, bytes);
    auto bytesOf = [&](uint64_t at, uint64_t n) {
        if (!n)
            return;
        room();
        p_.m(CE_SET_REMAP_CONST_A, value, SUBC_COPY);
        p_.m(CE_SET_REMAP_COMPONENTS, kFillConstA, SUBC_COPY);
        fillRun(at, uint32_t(n), 1, 1);
    };
    const uint64_t end = dst + bytes, first = std::min((dst + 3) & ~uint64_t(3), end),
                   last = std::max(end & ~uint64_t(3), first);
    bytesOf(dst, first - dst);
    if (last > first)
        fillWords(first, last - first, uint32_t(value) * 0x01010101u);
    bytesOf(last, end - last);
}

void BlitEncoder::copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width,
                           uint32_t lines, uint32_t bpp)
{
    cb_->bufferWrites_.note(dst, rowStorageBytes(dstPitch, width, lines, bpp));
    if (!width || !lines || !bpp)
        return;
    room();
    copyRows(p_, src, srcPitch, dst, dstPitch, width, lines, bpp, dev_->stats);
}

void BlitEncoder::copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                             uint32_t dy, uint32_t dz, uint32_t w, uint32_t h, uint8_t aspect)
{
    cb_->use(src, false);
    cb_->use(dst, true);
    room(32);
    const uint32_t remap = aspect == kAspectStencil ? remapOf(kRemapNoWrite, 1, kRemapNoWrite, kRemapNoWrite, 4, 2, 2)
                                                    : texelRemap(src.bpp);
    copyRemapped(p_, sideOf(src, sx, sy, sz), sideOf(dst, dx, dy, dz), w, h, remap,
                 aspect == kAspectStencil ? 4 : src.bpp, dev_->stats);
#if SWITCH_RAW_DIAGNOSTICS
    char detail[64];
    snprintf(detail, sizeof detail, " from %ux%u f%u at %u,%u", src.width, src.height, src.format, sx, sy);
    logEdgeMiss("copy", dst, {dx, dy, dx + w, dy + h}, detail);
#endif
}

void BlitEncoder::fillTexels(const Texture &t, uint32_t z, uint32_t w, uint32_t h, const CopyFill &fill)
{
    cb_->use(t, true);
    room(40);
    p_.m(CE_SET_REMAP_CONST_A, fill.constA, SUBC_COPY);
    p_.m(CE_SET_REMAP_CONST_B, fill.constB, SUBC_COPY);
    const CopySide side = sideOf(t, 0, 0, z);
    copyRemapped(p_, side, side, w, h, fill.remap, uint32_t(fill.componentBytes) * fill.components, dev_->stats);
}

void BlitEncoder::copyFirstByte(const Texture &from, const Texture &to)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    copyRemapped(p_, sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), std::min(from.width, to.width),
                 std::min(from.height, to.height),
                 remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, from.bpp, to.bpp), 1, dev_->stats);
}

void BlitEncoder::copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, uint32_t h, bool toTexture, uint8_t aspect)
{
    if (t.xrPlanes && t.luma)  // plane 0's words alone (raw_xr.h bufferTexelBytes): the alpha plane stays as it is
        return copyBufferTexels(va, pitch, *t.luma, x, y, z, w, h, toTexture, aspect);
    if (!toTexture)
        cb_->bufferWrites_.note(va, uint64_t(pitch) * h + uint64_t(w) * 16);
    cb_->use(t, toTexture);
#if SWITCH_RAW_DIAGNOSTICS
    if (toTexture && !t.zeta)
        logEdgeMiss("upload", t, {x, y, x + w, y + h}, "");
#endif
    room(48);
    const CopySide rows{va, pitch}, texels = sideOf(t, x, y, z);
    const CopySide &from = toTexture ? rows : texels, &to = toTexture ? texels : rows;
    if (t.zeta && t.format == kZetaFormat && aspect == kAspectDepth) {
        // the texel's first word: depth
        copyRemapped(p_, from, to, w, h,
                     toTexture ? remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 1, 2)
                               : remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 2, 1),
                     4, dev_->stats);
        return;
    }
    if (t.zeta && t.format == kZetaFormat && aspect == kAspectStencil && t.stencilTemp) {
        // its fifth byte, stencil: as the third of its two-byte components to or from the temp's two bytes a texel, and those
        // two bytes' first to or from the buffer's byte
        const Texture &s = *t.stencilTemp;
        cb_->use(s, true);
        const CopySide temp{s.va + uint64_t(y) * s.pitch + 2ull * x, s.pitch};
        if (toTexture) {
            copyRemapped(p_, rows, temp, w, h, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 1, 2), 1,
                         dev_->stats);
            copyRemapped(p_, temp, texels, w, h, remapOf(kRemapNoWrite, kRemapNoWrite, 0, kRemapNoWrite, 2, 1, 4), 2,
                         dev_->stats, true);
        } else {
            copyRemapped(p_, texels, temp, w, h, remapOf(2, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 2, 4, 1), 2,
                         dev_->stats);
            copyRemapped(p_, temp, rows, w, h, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 2, 1), 1,
                         dev_->stats, true);
        }
        return;
    }
    if (xrForm(t.format) == XrForm::Planes) {
        // four-byte components: the texel's color word to the buffer's word, or into the texel with its alpha word NO_WRITE (6)
        constexpr uint32_t kOut = 3u << 16 | 1u << 20, kIn = 6u << 4 | 3u << 16 | 1u << 24;
        copyRemapped(p_, from, to, w, h, toTexture ? kIn : kOut, bufferTexelBytes(t.format, t.bpp), dev_->stats);
        return;
    }
    copySides(p_, from, to, w, h, t.bpp, dev_->stats);
}

bool BlitEncoder::widenSrgbPair(const Texture &t)
{
    if (!srgbPair(t.format) || !t.standIn)
        return false;
    std::vector<XrPart> parts;
    if (!t.blockLinear) {
        XrPart whole;
        whole.codes = t;
        whole.standIn = *t.standIn;
        parts.push_back(whole);
    } else {
        Texture codes = t;
        codes.standIn = nullptr;
        if (!xrParts(codes, *t.standIn, parts))
            return false;
    }
    cb_->use(t, false);
    cb_->use(*t.standIn, true);
    // each texel's red and green bytes into the stand-in's first two, its blue and alpha NO_WRITE (the swizzle reads
    // zero and one there)
    const uint32_t widen = remapOf(0, 1, kRemapNoWrite, kRemapNoWrite, 1, 2, 4);
    for (const XrPart &part : parts) {
        room();
        copyRemapped(p_, sideOf(part.codes, 0, 0, 0), sideOf(part.standIn, 0, 0, 0), part.codes.width,
                     part.codes.height, widen, 2, dev_->stats);
    }
    return true;
}

// The codes Metal reads of RGB8_444_2P (pack/../m2v ycbcr_convert.rs: "Codes"): red Cr, green Y, blue Cb, each code / 255, alpha
// 1, an RGBA8Unorm texel of the codes, as m2v's image holds them. Remap per destination component: SRC_X..W 0..3, CONST_A 4,
// NO_WRITE 6 (clb0b5 SET_REMAP_COMPONENTS); one byte a component, four destination components.
void BlitEncoder::convertPlanar(const Texture &t)
{
    if (!t.planar || !t.width || !t.height)
        return;
    cb_->useAddress(t.planes.luma, rowStorageBytes(t.planes.lumaPitch, t.width, t.height, 1), false);
    cb_->useAddress(t.planes.chroma, rowStorageBytes(t.planes.chromaPitch, t.width, t.height, 2), false);
    cb_->use(t, true);
    RAW_DIAGNOSTIC(dev_->stats.ceOps += 2);
    RAW_DIAGNOSTIC(dev_->stats.ceBytes +=
                   uint64_t(t.width) * t.height * 4 * 2);  // (each launch writes the image's rows, through NO_WRITE)
    room();
    constexpr uint32_t A = 4, W = 6, one = 0 << 16, dst4 = 3u << 24;
    p_.m(CE_SET_REMAP_CONST_A, 0xff, SUBC_COPY);
    // the luma plane, one component a texel: green Y, alpha 255
    p_.m(CE_SET_REMAP_COMPONENTS, W | 0 << 4 | W << 8 | A << 12 | one | 0u << 20 | dst4, SUBC_COPY);
    p_.inc(CE_OFFSET_IN_UPPER,
           {uint32_t(t.planes.luma >> 32), uint32_t(t.planes.luma), uint32_t(t.va >> 32), uint32_t(t.va),
            t.planes.lumaPitch, t.pitch, t.width, t.height},
           SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kLaunchPitch | 1u << 10, SUBC_COPY);
    room();
    // the chroma plane, two components a texel (Cb, Cr): red Cr, blue Cb
    p_.m(CE_SET_REMAP_COMPONENTS, 1 | W << 4 | 0 << 8 | W << 12 | one | 1u << 20 | dst4, SUBC_COPY);
    p_.inc(CE_OFFSET_IN_UPPER,
           {uint32_t(t.planes.chroma >> 32), uint32_t(t.planes.chroma), uint32_t(t.va >> 32), uint32_t(t.va),
            t.planes.chromaPitch, t.pitch, t.width, t.height},
           SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kLaunchPitch | 1u << 10, SUBC_COPY);
}

void BlitEncoder::splitPlane(const Texture &t)
{
    if (!t.onePlane || !t.luma || !t.chroma || !t.chroma->width || !t.height)
        return;
    const uint32_t pairs = t.chroma->width;
    cb_->useAddress(t.planes.luma, rowStorageBytes(t.planes.lumaPitch, pairs, t.height, 4), false);
    cb_->use(*t.luma, true);
    cb_->use(*t.chroma, true);
    RAW_DIAGNOSTIC(dev_->stats.ceOps += 2);
    RAW_DIAGNOSTIC(dev_->stats.ceBytes += uint64_t(pairs) * t.height * 4);
    constexpr uint32_t one = 0 << 16, src4 = 3u << 20, dst2 = 1u << 24;
    for (const Texture *to : {t.luma, t.chroma}) {
        const uint32_t remap = to == t.luma ? 0 | 2 << 4 : 1 | 3 << 4;  // Y0, Y1 or Cb, Cr of Y0 Cb Y1 Cr
        room();
        p_.m(CE_SET_REMAP_COMPONENTS, remap | one | src4 | dst2, SUBC_COPY);
        p_.inc(CE_OFFSET_IN_UPPER,
               {uint32_t(t.planes.luma >> 32), uint32_t(t.planes.luma), uint32_t(to->va >> 32), uint32_t(to->va),
                t.planes.lumaPitch, to->pitch, pairs, t.height},
               SUBC_COPY);
        p_.m(CE_LAUNCH_DMA, kLaunchPitch | 1u << 10, SUBC_COPY);
    }
}

void BlitEncoder::access(const void *read, const void *write)
{
    auto in = [](const void *const *names, uint8_t n, const void *name) {
        return name && std::find(names, names + n, name) != names + n;
    };
    if (in(written_, writes_, read) || in(written_, writes_, write) || in(read_, reads_, write) ||
        reads_ == kAccesses || writes_ == kAccesses) {
        barrier();
        reads_ = writes_ = 0;
    }
    if (read && !in(read_, reads_, read))
        read_[reads_++] = read;
    if (write && !in(written_, writes_, write))
        written_[writes_++] = write;
}

void BlitEncoder::barrier()
{
    room(8);
    p_.m(WAIT_FOR_IDLE, 0);
    p_.inc(CE_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    constexpr uint32_t kNonPipelined = 2, kFlush = 1u << 2, kPitchToPitch = 1u << 7 | 1u << 8, kRemap = 1u << 10;
    p_.m(CE_LAUNCH_DMA, kNonPipelined | kFlush | kPitchToPitch | kRemap, SUBC_COPY);
}

void BlitEncoder::timestamp(uint64_t va, bool afterCopies)
{
    room(counters::kTimestampWords);
    cb_->useAddress(va, counters::kRecordBytes, true);
    counters::emitCopy(p_, va, afterCopies);
}

void BlitEncoder::generate(Texture &t, uint32_t fmt2d, uint32_t first, uint32_t last,
                           [[maybe_unused]] uint32_t unproven, uint32_t firstLayer, uint32_t layers)
{
    Texture::Chain *c = t.chain;
    if (!c || !fmt2d || first >= last || last >= c->layout.levels)
        return;
    cb_->use(t, true);
    room(uint32_t(fx::generateWords(c->layout, first, last) * layers));
    fx::emitGenerateLayers(p_, c->layout, c->va, fmt2d, first, last, firstLayer, layers);
    RAW_DIAGNOSTIC(dev_->stats.chainGenerates++);
    RAW_DIAGNOSTIC(dev_->stats.mipTwodLevels += last - first);
    RAW_DIAGNOSTIC(dev_->stats.mipTwodUnproven += unproven);
}

void BlitEncoder::end()
{
    if (p_.at == opened_)
        cb_->lastEngine_ = before_;
    checkRoom(p_, "a blit encoder");
    cb_->loops_.closeSegment(stream());
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    RAW_DIAGNOSTIC(dev_->stats.pushBytes += uint64_t(p_.at - start_) * 4);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static ProbeSurface probeSurface(const Texture &t, uint32_t fmt2d, uint32_t width, uint32_t height)
{
    ProbeSurface s;
    s.va = t.va;
    s.fmt2d = fmt2d;
    s.pitch = t.pitch;
    s.width = width;
    s.height = height;
    s.blockLinear = t.blockLinear;
    s.blockHeight = t.blockHeight;
    return s;
}

// The op's surfaces from the textures' memory at encode, which a private texture's move to plain memory may have changed
// since the op was recorded; its formats and sizes as recorded
void BlitEncoder::probe(const ProbeOp &recorded, const Texture *read, const Texture &written)
{
    ProbeOp op = recorded;
    if (op.kind != ProbeOpKind::Method) {
        op.dst = probeSurface(written, op.dst.fmt2d, op.dst.width, op.dst.height);
        if (read)
            op.src = probeSurface(*read, op.src.fmt2d, op.src.width, op.src.height);
    }
    room(uint32_t(probeOpWords(op)) + kProbeBracketWords);
    if (read)
        cb_->use(*read, false);
    cb_->use(written, true);
    if (op.bracketed) {
        tiledCacheDrain(p_, dev_->k_.info());
        p_.m(WAIT_FOR_IDLE, 0);
    }
    emitProbeOp(p_, op);
    if (op.bracketed)
        probeOpAfter(p_);
    cb_->taken_.ops++;
}

bool CommandBuffer::probeResolve(const Texture &multisampled, const Texture &into)
{
    fx::MipChain grid;
    const uint32_t fmt2d = probeTwodFormat(into.format);
    ProbeOp op;
    if (!fmt2d ||
        !fx::msLayout(multisampled.width, multisampled.height, multisampled.samples, multisampled.bpp, grid) ||
        !probeResolveOp(
            probeSurface(multisampled, fmt2d, multisampled.width << grid.sxLog2, multisampled.height << grid.syLog2),
            grid.sxLog2, grid.syLog2, probeSurface(into, fmt2d, into.width, into.height), op))
        return false;
    BlitEncoder *b = blit();
    b->probe(op, &multisampled, into);
    b->end();
    taken_.resolves++;
    return true;
}
#endif

}  // namespace raw
