// SPDX-License-Identifier: MIT
// See raw_gpu.h. The texture header fill follows Mesa NIL's nvb097_fill_image_view_desc (pitch, MIT).
#include "raw_lock.h"
#include "../fx/fx_image.h"
#include "raw_block_base.h"
#include "raw_gpu.h"
#include "raw_srgb_pair.h"
#include "../fx/fx_mip_probe.h"
#include "raw_multisample.h"
#include "TegraGM20BLayout.h"

#include <mach/mach.h>
#include <algorithm>
#include <cstring>

#include "raw_gpu_internal.h"

namespace raw {

uint32_t Device::allocTic()
{
    if (!freeTic_.empty()) {
        const uint32_t t = freeTic_.back();
        freeTic_.pop_back();
        return t;
    }
    return nextTic_ < kTicCount ? nextTic_++ : 0;
}

// A freed header holds header 0's null descriptor until it is reused: zeroes would read as a buffer texture at VA 0, which a
// draw still in flight, or one binding a stale index, would sample (a fault).
void Device::freeTic(uint32_t tic)
{
    memcpy(tic_.cpu + 32 * tic, tic_.cpu, 32);
    freeTic_.push_back(tic);
    views_.freed(tic);
    const auto freeViews = [&](auto &byHeader) {
        const auto views = byHeader.equal_range(tic);
        for (auto v = views.first; v != views.second; ++v) {
            memcpy(tic_.cpu + 32 * v->second.second, tic_.cpu, 32);
            freeTic_.push_back(v->second.second);
        }
        byHeader.erase(views.first, views.second);
    };
    freeViews(viewTics_);
    freeViews(attachmentViewTics_);
    // a part's header has view headers of its own (passViewTic): freed with it
    std::vector<uint32_t> parts;
    const auto made = partTics_.equal_range(tic);
    for (auto v = made.first; v != made.second; ++v)
        parts.push_back(v->second.second);
    partTics_.erase(made.first, made.second);
    for (const uint32_t part : parts)
        freeTic(part);
}

uint32_t Device::allocTicPair()
{
    if (!freeTicPairs_.empty()) {
        const uint32_t first = freeTicPairs_.back();
        freeTicPairs_.pop_back();
        return first;
    }
    if (nextTic_ + 1 >= kTicCount)
        return 0;
    const uint32_t first = nextTic_;
    nextTic_ += 2;
    return first;
}

// As freeTic leaves a freed header: header 0's null descriptor in both (an over-planes RGB10A8_2P_XR10 texture's pair,
// raw_xr_texture.cpp)
void Device::freeTicPair(uint32_t first)
{
    memcpy(tic_.cpu + 32 * first, tic_.cpu, 32);
    memcpy(tic_.cpu + 32 * (first + 1), tic_.cpu, 32);
    freeTicPairs_.push_back(first);
}

// Texture header of a pitch-linear 2D texture (Mesa NIL nvb097_fill_image_view_desc for a linear image: HEADER_VERSION PITCH,
// TEXTURE_TYPE TWO_D_NO_MIPMAP), word 0 NVK's for the format
void Device::writeTic(const Texture &t)
{
    fx::MipChain one;
    uint32_t w[8] = {};
    if (t.samples > 1) {  // NIL's image of the sample grid (createStoredMultisample, createOwnedZeta, their views)
        const uint32_t z = zetaFormatIndex(t.format);
        const FormatInfo *f = z < kZetaFormats ? nullptr : formatInfo(t.format);
        multisampleHeader(t.width, t.height, t.samples, t.bpp, t.va, f ? f->tic0 : kZetaFormatInfo[z].tic0,
                          f && f->srgb, w);
    } else if (t.zeta && t.layers > 1 && t.type != fx::kTexTwoD && zetaArrayHeader(t, t.type, w)) {
        // an owned depth array, cube or cube array (createOwnedZeta) as the type it was made
    } else if (t.zeta) {  // NVK's image as laid out (createZetaOver): one level, its block height
        one.width = t.width;
        one.height = t.height;
        one.levels = 1;
        one.bpp = t.bpp;
        one.yLog2 = t.blockHeight;
        fx::mipChainTic(one, t.va, kZetaFormatInfo[zetaFormatIndex(t.format)].tic0, false, 0, 1, w, fx::kTexTwoD);
    } else if (t.type == fx::kTexOneDBuffer) {
        const FormatInfo *f = formatInfo(t.format);
        fx::bufferTic(t.va, f->tic0, f->srgb, t.width, w);
    } else if (const FormatInfo *f = formatInfo(t.format);
               (t.layers > 1 && fx::arrayLayout(t.width, t.height, t.layers, 1, t.bpp, one)) ||
               (t.type == fx::kTexThreeD &&
                fx::volumeLayout(t.width, t.height, t.depth, 1, t.bpp, !t.blockDepth, one)) ||
               (t.blockLinear && fx::mipChainLayout(t.width, t.height, 1, t.bpp, one))) {  // NIL's header of the image
        if (t.blockLinear || t.blockW > 1 || t.blockH > 1)
            one.yLog2 = t.blockHeight;
        fx::mipChainTic(one, t.va, f->tic0, f->srgb, 0, 1, w, t.type);
    } else {
        w[0] = f->tic0;
        setField(w, 87, 85, 2);                        // HEADER_VERSION PITCH
        setField(w, 63, 37, (t.va >> 5) & 0x7ffffff);  // ADDRESS_BITS31TO5
        setField(w, 79, 64, (t.va >> 32) & 0xffff);
        setField(w, 111, 96, (t.pitch >> 5) & 0xffff);
        setField(w, 154, 151, 7);  // TWO_D_NO_MIPMAP
        setField(w, 112, 112, 1);  // LOD_ANISO_QUALITY2
        setField(w, 113, 113, 1);  // LOD_ANISO_QUALITY HIGH
        setField(w, 114, 114, 1);  // LOD_ISO_QUALITY HIGH
        setField(w, 143, 128, t.width - 1);
        setField(w, 175, 160, t.height - 1);
        setField(w, 150, 150, f->srgb);
        setField(w, 159, 157, 7);  // BORDER_SIZE BORDER_SAMPLER_COLOR
        setField(w, 191, 191, 1);  // NORMALIZED_COORDS
        setField(w, 216, 215, 2);  // ANISO_FINE_SPREAD_FUNC TWO
        setField(w, 218, 217, 1);  // ANISO_COARSE_SPREAD_FUNC ONE
    }
    if (t.blockW > 1 || t.blockH > 1) {
        setField(w, 143, 128, t.texelWidth - 1);  // WIDTH_MINUS_ONE in texels (NIL's extent_px)
        setField(w, 175, 160, t.texelHeight - 1);
    }
    w[0] = swizzledWord0(w[0], t.swizzle);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // write-combined: visible at the next submission's header invalidate
}

void Device::swizzle(Texture &t, const uint8_t channels[4])
{
    std::lock_guard<raw::Mutex> l(lock_);
    memcpy(t.swizzle, channels, 4);
    if (!t.tic)
        return;
    uint32_t *w = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic);
    w[0] = swizzledWord0(w[0], t.swizzle);
    // a draw reads its stand-in (raw_bound_texture.h): the swizzle over the stand-in's own (an RG8Unorm_sRGB one's red,
    // green, zero, one)
    if (t.standIn && t.standIn->tic) {
        uint32_t *s = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.standIn->tic);
        s[0] = swizzledWord0(s[0], channels);
    }
}

uint32_t swizzledWord0(uint32_t word0, const uint8_t channels[4])
{
    constexpr uint32_t kZero = 0, kOneInt = 6, kOneFloat = 7, kSint = 3, kUint = 4;
    auto source = [&](uint32_t c) {
        return word0 >> (19 + 3 * c) & 7;
    };
    const uint32_t redType = word0 >> 7 & 7;
    uint32_t one = redType == kSint || redType == kUint ? kOneInt : kOneFloat;
    for (uint32_t c = 0; c < 4; c++)
        if (source(c) == kOneInt || source(c) == kOneFloat)
            one = source(c);
    uint32_t out = word0 & ~(0xfffu << 19);
    for (uint32_t c = 0; c < 4; c++) {
        const uint8_t s = channels[c];  // MTLTextureSwizzle: Zero, One, Red, Green, Blue, Alpha
        const uint32_t from = s == 0 ? kZero : s == 1 ? one : s <= 5 ? source(s - 2u) : source(c);
        out |= from << (19 + 3 * c);
    }
    return out;
}

bool shape(uint32_t width, uint32_t height, uint32_t format, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    if (!f || !width || !height || width > 16384 || height > 16384)
        return false;
    t = {};
    t.blockW = f->blockW;
    t.blockH = f->blockH;
    t.texelWidth = width;
    t.texelHeight = height;
    t.width = (width + f->blockW - 1) / f->blockW;
    t.height = (height + f->blockH - 1) / f->blockH;
    t.format = format;
    t.bpp = f->bpp;
    t.renderable = f->rt != 0;
    return true;
}

// NIL's layout of a block-compressed image: its blocks tiled, the block height chosen by its height in texels (Tiling::choose
// reads extent_px) and clamped to its rows of blocks
static void blockLayout(const Texture &t, tgm_layout &bl)
{
    const uint8_t y =
        uint8_t(std::min<uint32_t>(tgm_layout_y_log2(t.texelHeight), tgm_layout_log2_ceil((t.height + 7) / 8)));
    const uint64_t block = 512ull << y;
    bl.y_log2 = y;
    bl.rows = (t.height + (8u << y) - 1) / (8u << y) * (8u << y);
    bl.align = std::max<uint64_t>(block, 4096);
    bl.bytes = (uint64_t(bl.row_bytes) * bl.rows + bl.align - 1) / bl.align * bl.align;
}

// createTexture's block-linear layout of shaped `t`, where it is asked tiled and its shape tiles: every kFormats format, a
// colour one under colour kind 0xFE (one the GM20B renders none of too, as its chains are) or a block-compressed one
// (NVK's optimal image)
static bool plainTiled(const Texture &t, bool tiled, tgm_layout &bl)
{
    const bool blocks = t.blockW > 1 || t.blockH > 1;
    if (!tiled || !tgm_layout_tiled(t.width, t.height, t.bpp, &bl))
        return false;
    if (blocks)
        blockLayout(t, bl);
    return true;
}

static void tiledRows(Texture &t, const tgm_layout &bl)
{
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
}

static void chainRows(Texture &t, const fx::MipChain &c, uint64_t layerBytes)
{
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = layerBytes;
}

bool Device::createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc, bool tiled,
                           const ArenaAt *at)
{
    if (at) {
        TextureAsk ask;
        ask.width = width;
        ask.height = height;
        ask.format = format;
        ask.tiled = tiled;
        return createLaidOut(ask, t, at);
    }
    if (!shape(width, height, format, t))
        return false;
    const bool blocks = t.blockW > 1 || t.blockH > 1;
    t.pitch = (t.width * t.bpp + 255) & ~255u;
    std::lock_guard<raw::Mutex> l(lock_);
    const uint64_t bytes = uint64_t(t.pitch) * t.height;
    tgm_layout bl;
    const bool laid = plainTiled(t, tiled, bl);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeCompressNext.exchange(false) && laid && !wc && !blocks && compressedKind(t.bpp, 0))
        return probeCompressible(bl, t);
#endif
    if (laid && blAlloc(bl.bytes, bl.align, t.span)) {
        tiledRows(t, bl);
    } else if (blocks) {
        return false;
    } else if (wc && wcAlloc(bytes, t.span)) {
        t.wc = true;
        RAW_DIAGNOSTIC(stats.wcBytes += t.span.bytes);
    } else {
        if (wc)
            RAW_DIAGNOSTIC(stats.wcFallbacks++);
        if (!poolAlloc(bytes, t.span))
            return false;
    }
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, memoryOf(t), none);
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::layout(const TextureAsk &ask, Texture &t, uint64_t &bytes, uint64_t &align, uint32_t &kind,
                    fx::MipChain *chain)
{
    fx::MipChain own;
    fx::MipChain &c = chain ? *chain : own;
    const FormatInfo *f = formatInfo(ask.format);
    kind = fx::kBlockLinearKind;
    switch (ask.kind) {
    case TextureAsk::Plain: {
        tgm_layout bl;
        if (!shape(ask.width, ask.height, ask.format, t))
            return false;
        if (plainTiled(t, ask.tiled, bl)) {
            tiledRows(t, bl);
            bytes = bl.bytes;
            align = bl.align;
            return true;
        }
        if (t.blockW > 1 || t.blockH > 1)
            return false;
        t.pitch = (t.width * t.bpp + 255) & ~255u;
        bytes = uint64_t(t.pitch) * t.height;
        align = 256;
        kind = kPteKindPitch;
        return true;
    }
    case TextureAsk::Line: {
        tgm_layout bl;
        if (!shape(ask.width, 1, ask.format, t) || !tgm_layout_tiled(ask.width, 1, t.bpp, &bl))
            return false;
        tiledRows(t, bl);
        t.type = fx::kTexOneD;
        bytes = bl.bytes;
        align = bl.align;
        return true;
    }
    case TextureAsk::Mips:
        if (!f || ask.levels < 2 || blockCompressed(f) ||
            !fx::mipChainLayout(ask.width, ask.height, ask.levels, f->bpp, c) ||
            !shape(ask.width, ask.height, ask.format, t))
            return false;
        chainRows(t, c, c.bytes);
        break;
    case TextureAsk::Volume:
        if (!shape(ask.width, ask.height, ask.format, t) ||
            !fx::volumeLayout(ask.width, ask.height, ask.depth, ask.levels, t.bpp, ask.view2d, c))
            return false;
        chainRows(t, c, c.bytes);
        t.type = fx::kTexThreeD;
        t.depth = ask.depth;
        t.blockDepth = c.zLog2;
        break;
    case TextureAsk::Array:
        if (!shape(ask.width, ask.height, ask.format, t) ||
            !fx::arrayLayout(ask.width, ask.height, ask.layers, ask.levels, t.bpp, c))
            return false;
        chainRows(t, c, c.arrayStride);
        t.type = ask.type;
        t.layers = ask.layers;
        break;
    case TextureAsk::Multisample:
        if (!shape(ask.width, ask.height, ask.format, t) || blockCompressed(f) ||
            !fx::msLayout(t.width, t.height, ask.samples, t.bpp, c))
            return false;
        chainRows(t, c, uint64_t(c.level[0].rowBytes) * c.level[0].rows);
        t.samples = uint8_t(ask.samples);
        break;
    case TextureAsk::Zeta:
        if (!zetaImageLayout(ask.width, ask.height, ask.format, ask.samples, ask.layers, ask.levels, ask.type,
                             ask.sampled, t, c, bytes, align))
            return false;
        align = std::max(align, kBlAlign);
        kind = kZetaFormatInfo[zetaFormatIndex(ask.format)].kind;
        return true;
    case TextureAsk::Block:
        if (!blockCompressed(f) || !shape(ask.width, ask.height, ask.format, t) ||
            !(ask.type == fx::kTexThreeD ? fx::volumeLayout(ask.width, ask.height, ask.depth, ask.levels, f->bpp, false,
                                                            c, f->blockW, f->blockH)
                                         : fx::blockChainLayout(ask.width, ask.height, ask.levels, ask.layers,
                                                                f->blockW, f->blockH, f->bpp, c)))
            return false;
        chainRows(t, c, c.arrayStride);
        break;
    }
    bytes = c.bytes;
    align = c.align;
    return true;
}

// An arena's block-linear memory at NVK's image alignment, as m2v's twin over it in place takes it
static void arenaRounded(uint32_t kind, uint64_t &bytes, uint64_t &align)
{
    if (kind == kPteKindPitch)
        return;
    bytes = (bytes + kBlAlign - 1) & ~(kBlAlign - 1);
    align = std::max(align, kBlAlign);
}

bool Device::footprint(const TextureAsk &ask, uint64_t &bytes, uint64_t &align)
{
    Texture t;
    uint32_t kind = 0;
    if (!layout(ask, t, bytes, align, kind))
        return false;
    arenaRounded(kind, bytes, align);
    if (!ask.standIn)
        return true;
    uint64_t standIn = 0, standInAlign = 0;
    if (!footprint(standInAsk(t), standIn, standInAlign))
        return false;
    bytes = (bytes + standInAlign - 1) / standInAlign * standInAlign + standIn;
    align = std::max(align, standInAlign);
    return true;
}

// Its codes' shape (raw_xr_parts.h converts the pair texel for texel): a one-level 2D texture's tiled, else the kind
// and levels, layers, planes or samples of the texture's own layout
TextureAsk Device::standInAsk(const Texture &t)
{
    TextureAsk ask;
    ask.width = t.width;
    ask.height = t.height;
    ask.format = srgbPair(t.format) ? kSrgbPairStandInFormat : kXrStandInFormat;
    ask.tiled = true;
    ask.levels = t.chain ? t.chain->layout.levels : 1;
    if (t.samples > 1) {
        ask.kind = TextureAsk::Multisample;
        ask.samples = t.samples;
    } else if (t.type == fx::kTexThreeD) {
        ask.kind = TextureAsk::Volume;
        ask.depth = t.depth;
        ask.view2d = t.blockDepth == 0;
    } else if (t.type == fx::kTexOneD) {
        ask.kind = TextureAsk::Line;
    } else if (t.type != fx::kTexTwoD || t.layers > 1) {
        ask.kind = TextureAsk::Array;
        ask.type = t.type;
        ask.layers = std::max(t.layers, 1u);
    } else if (t.chain) {
        ask.kind = TextureAsk::Mips;
    }
    return ask;
}

bool Device::createLaidOut(const TextureAsk &ask, Texture &t, const ArenaAt *at)
{
    uint64_t bytes = 0, align = 0;
    uint32_t kind = 0;
    Texture::Chain *chain = ask.kind == TextureAsk::Mips ? new Texture::Chain : nullptr;
    const bool image = ask.kind == TextureAsk::Volume || ask.kind == TextureAsk::Array || ask.kind == TextureAsk::Block;
    const bool zetaLevels = ask.kind == TextureAsk::Zeta && ask.levels > 1;
    fx::MipChain imageLayout;
    if ((!at && ask.kind == TextureAsk::Plain) || !layout(ask, t, bytes, align, kind,
                                                          chain                 ? &chain->layout
                                                          : image || zetaLevels ? &imageLayout
                                                                                : nullptr)) {
        delete chain;
        return false;
    }
    if (at)
        arenaRounded(kind, bytes, align);
    const bool zeta = ask.kind == TextureAsk::Zeta;
    const uint32_t z = zetaFormatIndex(ask.format);
    std::lock_guard<raw::Mutex> l(lock_);
    Span owned;
    Bo comp;
    const bool compressed = ask.compressKind && !at && !zeta && takeCompressed(bytes, ask.compressKind, comp);
    const bool placed = compressed || (at     ? arenaSpan(*at, bytes, align, kind, t)
                                       : zeta ? tiledAlloc(zChunks_[z], kind, bytes, align, owned)
                                              : blAlloc(bytes, align, t.span));
    if (!placed) {
        delete chain;
        return false;
    }
    const Span &span = zeta && !at ? owned : t.span;
    if ((!zeta || ask.sampled || ask.samples > 1) && !(t.tic = allocTic())) {
        Lease *none = nullptr;
        if (compressed)
            comp_.ready.push_back({comp, ask.compressKind, commits(), compClock()});
        else if (zeta && !at)
            tiledFree(zChunks_[z], owned);
        else
            freeSpan(t.span, memoryOf(t), none);
        delete chain;
        return false;
    }
    t.va = span.va;
    if (compressed) {
        noteCompressed(ask, comp, bytes, t);
    } else if (zeta) {
        t.storage = {span.va, bytes, false, span.storage};
        if (!at)
            ownedZeta_[span.va] = {z, owned};
    } else {
        t.cpu = span.cpu;
    }
    if (chain) {
        const FormatInfo *f = formatInfo(ask.format);
        chain->va = t.va;
        chain->fmt2d = fx::twodChainFormat(ask.format);
        uint32_t w[8];
        fx::mipChainTic(chain->layout, chain->va, f->tic0, f->srgb, 0, ask.levels, w);
        // the texture's header: the chain's (visible at the next header invalidate)
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
        t.chain = chain;
        RAW_DIAGNOSTIC(stats.chains++);
        RAW_DIAGNOSTIC(stats.chainBytes += chain->layout.bytes);
    } else if (ask.kind == TextureAsk::Block) {
        layBlocks(imageLayout, formatInfo(ask.format), ask.width, ask.height, ask.type, t);
    } else if (image) {
        layImage(imageLayout, formatInfo(ask.format), t.type, t);
    } else if (zetaLevels) {
        t.chain = new Texture::Chain;
        t.chain->layout = imageLayout;
        t.chain->va = t.va;
        if (t.tic) {
            uint32_t w[8];
            fx::mipChainTic(imageLayout, t.va, kZetaFormatInfo[z].tic0, false, 0, ask.levels, w, t.type);
            memcpy(tic_.cpu + 32 * t.tic, w, 32);
        }
    } else if (t.tic) {
        writeTic(t);
    }
    return true;
}

bool Device::createLine(uint32_t width, uint32_t format, Texture &t, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Line;
    ask.width = width;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createVolume(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, bool view2d, Texture &t,
                          uint32_t levels, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Volume;
    ask.width = width;
    ask.height = height;
    ask.depth = depth;
    ask.levels = levels;
    ask.format = format;
    ask.view2d = view2d;
    return createLaidOut(ask, t, at);
}

bool Device::createArray(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t type, Texture &t,
                         uint32_t levels, const ArenaAt *at, uint32_t compressKind)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Array;
    ask.compressKind = compressKind;
    ask.width = width;
    ask.height = height;
    ask.layers = layers;
    ask.levels = levels;
    ask.format = format;
    ask.type = uint8_t(type);
    return createLaidOut(ask, t, at);
}

void Device::layImage(const fx::MipChain &c, const FormatInfo *f, uint8_t type, Texture &t)
{
    const bool volume = type == fx::kTexThreeD;
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.type = type;
    t.layers = volume ? 1 : c.layers;
    t.layerBytes = volume ? c.bytes : c.arrayStride;
    t.depth = c.depth;
    t.blockDepth = c.zLog2;
    if (c.levels == 1) {
        writeTic(t);
        return;
    }
    t.chain = new Texture::Chain;
    t.chain->layout = c;
    t.chain->va = t.va;
    t.chain->fmt2d = volume ? 0 : fx::twodChainFormat(t.format);
    uint32_t w[8];
    fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, c.levels, w, type);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
    RAW_DIAGNOSTIC(stats.chains++);
    RAW_DIAGNOSTIC(stats.chainBytes += c.bytes);
}

bool Device::createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t,
                              const ArenaAt *at, uint32_t compressKind)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Mips;
    ask.compressKind = compressKind;
    ask.width = width;
    ask.height = height;
    ask.levels = levels;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createBlockTextureOver(void *bytes, uint64_t length, uint32_t width, uint32_t height, uint32_t format,
                                    const fx::MipChain &layout, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    if (!blockCompressed(f) || !bytes || length < layout.bytes || layout.bpp != f->bpp ||
        !shape(width, height, format, t))
        return false;
    const bool inPlace = blockImageInPlace(uintptr_t(bytes), layout);
    const uint32_t kind = inPlace ? TGM_LAYOUT_KIND : kPteKindPitch;
    cleanToMemory(bytes, layout.bytes);
    Import *i = new Import;
    if (!k_.importPages(bytes, length, i->bo, 0, true)) {
        delete i;
        return false;
    }
    std::lock_guard<raw::Mutex> l(lock_);
    if (!k_.bindImport(bytes, i->bo, kind)) {
        delete i;
        return false;
    }
    i->kind = kind;
    i->refs = 1;
    i->base = uintptr_t(bytes);
    i->bytes = length;
    RAW_DIAGNOSTIC(stats.imports++);
    RAW_DIAGNOSTIC(stats.importsMade++);
    RAW_DIAGNOSTIC(stats.importBytes += i->bo.bytes);
    if (!inPlace && !blAlloc(layout.bytes, layout.align, t.span)) {
        release(i);
        return false;
    }
    if ((!inPlace && !(t.imagePitchVa = blPitchVa(t.span))) || !(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, Memory::Bl, none);
        release(i);
        return false;
    }
    if (inPlace) {
        t.import = i;
        t.va = i->bo.va;
        t.cpu = static_cast<uint8_t *>(bytes);
    } else {
        t.callerPages = i;
        t.va = t.span.va;
        t.cpu = t.span.cpu;
    }
    t.storage = {t.va, layout.bytes, inPlace};
    layBlocks(layout, f, width, height, uint8_t(layout.layers > 1 ? fx::kTexTwoDArray : fx::kTexTwoD), t);
    return true;
}

bool Device::createBlockTexture(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, uint32_t format,
                                uint8_t type, Texture &t, uint32_t depth, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Block;
    ask.width = width;
    ask.height = height;
    ask.depth = depth;
    ask.levels = levels;
    ask.layers = layers;
    ask.format = format;
    ask.type = type;
    return createLaidOut(ask, t, at);
}

void Device::layBlocks(const fx::MipChain &layout, const FormatInfo *f, uint32_t width, uint32_t height, uint8_t type,
                       Texture &t)
{
    t.pitch = layout.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = layout.yLog2;
    t.layers = layout.layers;
    t.layerBytes = type == fx::kTexThreeD ? layout.bytes : layout.arrayStride;
    t.depth = layout.depth;
    t.blockDepth = layout.zLog2;
    t.type = type;
    uint32_t w[8];
    fx::mipChainTic(layout, t.va, f->tic0, f->srgb, 0, layout.levels, w, t.type);
    setField(w, 143, 128, width - 1);  // WIDTH_MINUS_ONE in texels (NIL's extent_px)
    setField(w, 175, 160, height - 1);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);
    if (layout.levels > 1) {
        t.chain = new Texture::Chain;
        t.chain->layout = layout;
        t.chain->va = t.va;
    }
}

void Device::destroyTexture(Texture &t)
{
#if SWITCH_RAW_DIAGNOSTICS
    if (glassMemOn.load(std::memory_order_relaxed))
        glassMem->forget(t.storageRange().va ? t.storageRange().va : t.va, t.view ? t.tic : 0);
#endif
    if (t.view) {
        std::lock_guard<raw::Mutex> l(lock_);
        forgetView(t);
        if (t.tic) {
            freeTic(t.tic);
        }
        t = {};
        return;
    }
    for (Texture *plane : {t.luma, t.chroma, t.planarStandIn, t.stencilTemp})
        if (plane) {
            destroyTexture(*plane);
            delete plane;
        }
    t.luma = t.chroma = t.planarStandIn = t.stencilTemp = nullptr;
    delete t.ycbcrStore;
    t.ycbcrStore = nullptr;
    if (t.standIn) {
        destroyTexture(*t.standIn);
        delete t.standIn;
        t.standIn = nullptr;
    }
    if (t.sampled) {  // its header is the pool image's
        destroyTexture(*t.sampled);
        delete t.sampled;
        t.sampled = nullptr;
        t.tic = 0;
    }
    std::lock_guard<raw::Mutex> l(lock_);
    if (t.tic && t.xrPlanes) {
        freeTicPair(t.tic);
    } else if (t.tic) {
        freeTic(t.tic);
    }
    if (t.zeta)
        freeOwnedZeta(t.va);
    if (t.chain) {
        for (const uint32_t *tics : {t.chain->levelTic, t.chain->levelCodesTic})
            for (uint32_t level = 0; level < fx::kMaxLevels; level++)
                if (tics[level])
                    freeTic(tics[level]);
        delete t.chain;
        t.chain = nullptr;
    }
    if (t.import)
        release(t.import);
    if (t.callerPages)
        release(t.callerPages);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (auto it = probeCompressed.find(t.va); t.comptags && !t.span.valid() && it != probeCompressed.end()) {
        k_.free(it->second);
        probeCompressed.erase(it);
    }
#endif
    releasePrivateCompressed(t);
    freeSpan(t.span, memoryOf(t), t.lease);
    t = {};
}

bool Device::createStandIn(Texture &t, const ArenaAt *at)
{
    const bool pair = srgbPair(t.format);
    if (t.standIn || (!pair && !xrPipeline(xrForm(t.format), t.xrPlanes)))
        return t.standIn != nullptr;
    Texture *s = new Texture;
    const TextureAsk ask = standInAsk(t);
    const bool made = ask.kind == TextureAsk::Plain ? createTexture(t.width, t.height, ask.format, *s, false, true, at)
                                                    : createLaidOut(ask, *s, at);
    if (!made) {
        delete s;
        return false;
    }
    if (pair)
        swizzle(*s, kSrgbPairSwizzle);
    t.standIn = s;
    return true;
}

Texture::StorageRange Texture::storageRange() const
{
    if (storage.bytes)
        return storage;
    if (span.bytes)
        return {span.va, span.bytes, false, span.storage};
    if (chain)
        return {chain->va, chain->layout.bytes, false};
    if ((blockLinear || nativeGeneration) && layerBytes && layers && layerBytes <= UINT64_MAX / layers)
        return {va, layerBytes * layers, false};
    return {};
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
bool Device::probeRetarget(const Texture &a, const Texture *to)
{
    std::lock_guard<raw::Mutex> l(lock_);
    if (!a.tic || (to && !to->tic))
        return false;
    if (to)
        memcpy(tic_.cpu + 32 * a.tic, tic_.cpu + 32 * to->tic, 32);
    else
        writeTic(a);
    return true;
}

bool Device::probeCompressible(const tgm_layout &bl, Texture &t)
{
    const uint32_t kind = compressedKind(t.bpp, 0);
    Bo bo;
    if (k_.allocCompressible(compBytes(bl.bytes), bo) != KERN_SUCCESS)
        return false;
    if (k_.bindCompressed(bo, kind) != KERN_SUCCESS) {
        k_.free(bo);
        return false;
    }
    t.va = bo.va;
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
    t.comptags = true;
    if (!(t.tic = allocTic())) {
        k_.free(bo);
        return false;
    }
    probeCompressed[bo.va] = bo;
    writeTic(t);
    return true;
}
#endif

}  // namespace raw
