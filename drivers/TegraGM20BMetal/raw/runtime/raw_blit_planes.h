// SPDX-License-Identifier: MIT
// What Metal's blits move of a texture held through a surface's planes (m2v copy_plan.rs, blit_plan.rs, two_planes.rs;
// EVs XRR2-ycc2-m4, TXN2-ycr2-m4 and XRR2-xrs1-m4 on an Apple M4 under macOS 27.0.1; an M4 Max under macOS 27.2.1,
// validation off): plane 0's bytes, planeUnit of them a texel, which a blit with a buffer moves, and a blit with a
// texture of another format counts in the destination's texel (into the planes: only when the two match). A blit
// between two such textures moves plane 0 so and, where both have one, plane 1 too, which Metal moves at plane 0's
// region in bytes and rows, past the plane's rows: the raw path moves the part of each plane 1 the region covers
// instead (planeOneSpan). Counted in a texel wider than plane 0's unit, a region runs along plane 0 as one run of
// bytes, texel x of row y at y * pitch + x * texel, through each row's padding into the next rows and past plane 0 into
// the surface's bytes after it, up to the end of its last page, past which an M4 Max reads 0 (planeReach; m2v-blits-fix
// probes/bound.mm, macOS 27.2.1).
#pragma once
#include <algorithm>
#include <cstdint>
#include "raw_gpu.h"

namespace raw {

// A one-plane 4:4:4 YCbCr texture (504, 524): luma and chroma both read its one plane of words (raw_ycbcr.h
// chromaGreenBlue), as RGB10A2Unorm
inline bool planeOfWords(const Texture &t)
{
    constexpr uint32_t kRGB10A2Unorm = 90;
    return t.ycbcr && !t.onePlane && t.luma && t.luma->format == kRGB10A2Unorm;
}

// The bytes of a texel of plane 0 that Metal's blits with a buffer, and with a texture of plane 0's texel size, count:
// a code's (1 or 2) for two-plane YCbCr and 4:4:4 codes, the word's (4) for RGB10A8_2P_XR10 and one-plane 4:4:4, one
// for packed 10-bit planes, two for one-plane 4:2:2; 0: not held through planes
inline uint32_t planeUnit(const Texture &t)
{
    constexpr uint32_t kR16Unorm = 20;
    if (t.xrPlanes || planeOfWords(t))
        return 4;
    if (t.onePlane)
        return 2;
    if (t.planar)
        return 1;
    if (t.ycbcr && t.luma)
        return t.luma->format == kR16Unorm ? 2 : 1;
    return 0;
}

// Plane 0 of `t`'s rows as texels of `unit` bytes (1, 2, 4, 8 or 16), pitch memory: what Metal's blits of a texture held
// through planes move; false: no such plane
inline bool planeBytes(const Texture &t, uint32_t unit, Texture &out)
{
    constexpr uint32_t kUnitFormats[5] = {10, 23, 53, 103, 123};  // R8Unorm, R16Uint, R32Uint, RG32Uint, RGBA32Uint
    const Texture *luma = t.xrPlanes || (t.ycbcr && !t.onePlane) ? t.luma : nullptr;
    const uint64_t va = luma ? luma->va : t.planes.luma;
    const uint32_t pitch = luma ? luma->pitch : t.planes.lumaPitch, rows = luma ? luma->height : t.height;
    Import *import = luma ? luma->import : t.import;
    const uint32_t log2 = unit ? uint32_t(__builtin_ctz(unit)) : 5;
    if (!va || !pitch || !rows || !import || log2 > 4 || unit != 1u << log2 || pitch < unit ||
        (luma && luma->blockLinear))
        return false;
    out = Texture();
    out.import = import;
    out.va = va;
    out.pitch = pitch;
    out.bpp = unit;
    out.format = kUnitFormats[log2];
    out.width = out.texelWidth = pitch / unit;
    out.height = out.texelHeight = rows;
    out.storage = {va, uint64_t(pitch) * (rows - 1) + uint64_t(out.width) * unit, !import->id};
    return true;
}

// The bytes from plane 0's first that a blit of `w` x `h` of `plane`'s texels from (x, y) reaches (planeBytes' plane)
inline uint64_t planeReach(const Texture &plane, uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    return w && h ? (y + h - 1) * plane.pitch + (x + w) * plane.bpp : 0;
}

// The bytes from plane 0's first to the end of its surface's last page, as its import binds them
inline uint64_t planeReadable(const Texture &plane)
{
    const Bo &bo = plane.import->bo;
    return (bo.vaBase ? bo.vaBase : bo.va) + bo.bytes - plane.va;
}

// planeBytes, its storage what a blit of `w` x `h` of its texels from (x, y) reads of the surface (planeReach)
inline bool planeBytesRead(const Texture &t, uint32_t unit, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           Texture &out)
{
    if (!planeBytes(t, unit, out))
        return false;
    out.storage.bytes = std::max(out.storage.bytes, planeReach(out, x, y, w, h));
    return true;
}

// The span of plane 1 (`size` of plane 0's units from `origin`) the same region covers: a subsampled chroma plane's
// pairs, rounded out to the pairs the region's texels fall in
inline void chromaSpan(uint32_t lumaSide, uint32_t chromaSide, uint32_t origin, uint32_t size, uint32_t &from,
                       uint32_t &count)
{
    const uint32_t step = chromaSide && chromaSide < lumaSide ? (lumaSide + chromaSide - 1) / chromaSide : 1;
    from = origin / step;
    count = std::min((origin + size + step - 1) / step, chromaSide) - std::min(from, chromaSide);
}

// Plane 1 of `t` (its chroma pairs, or RGB10A8_2P_XR10's alpha bytes) as bytes over its rows, and the bytes
// [bx, bx + bw) of rows [by, by + bh) of it that plane 0's [x, x + w) x [y, y + h) covers: the pairs its texels fall in
// (chromaSpan), a packed plane's whole words of them (three codes a word), the alpha bytes texel for texel; false: no
// plane 1 (a one-plane format's chroma reads plane 0)
inline bool planeOneSpan(const Texture &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, Texture &out, uint32_t &bx,
                         uint32_t &by, uint32_t &bw, uint32_t &bh)
{
    constexpr uint32_t kPackedWords = 53;  // R32Uint: a packed plane's texture counts words
    const Texture *c = (t.xrPlanes || (t.ycbcr && !t.onePlane && t.luma && !planeOfWords(t))) ? t.chroma : nullptr;
    uint64_t va = c ? c->va : t.planes.chroma;
    uint32_t pitch = c ? c->pitch : t.planes.chromaPitch, rows = c ? c->height : t.height,
             pairs = c ? c->width : t.width, pairBytes = c ? c->bpp : 2;
    Import *import = c ? c->import : t.import;
    const bool packed = c && !t.xrPlanes && c->format == kPackedWords;
    if (packed) {  // the YCbCr shape's halved axes (raw_ycbcr.h): bit 0 across
        pairs = t.shape[13] & 1 ? (t.width + 1) / 2 : t.width;
        pairBytes = 0;
    }
    if ((!c && !t.planar) || !va || !pitch || !rows || !pairs || !import || (c && c->blockLinear))
        return false;
    uint32_t px, pw;
    chromaSpan(t.width, pairs, x, w, px, pw);
    chromaSpan(t.height, rows, y, h, by, bh);
    if (packed) {
        const uint32_t first = 2 * px / 3, end = std::min((2 * (px + pw) + 2) / 3, c->width);
        bx = 4 * first;
        bw = end > first ? 4 * (end - first) : 0;
    } else {
        bx = px * pairBytes;
        bw = pw * pairBytes;
    }
    out = Texture();
    out.import = import;
    out.va = va;
    out.pitch = pitch;
    out.bpp = 1;
    out.format = 10;  // R8Unorm
    out.width = out.texelWidth = pitch;
    out.height = out.texelHeight = rows;
    out.storage = {va, uint64_t(pitch) * rows, !import->id};
    return true;
}

// Whether a blit moves `t`'s planes: one held through a surface's planes or through its two extended-range planes
inline bool heldInPlanes(const Texture &t)
{
    return t.converted() || t.xrPlanes;
}

}  // namespace raw
