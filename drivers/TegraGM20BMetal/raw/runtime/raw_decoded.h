// SPDX-License-Identifier: MIT
// The formats GM20B's texture unit decodes none of that the identities make: PVRTC (160-167), Metal's ASTC footprints
// of 4x2 and 8x4 texels (185, 191, 203, 209) and ASTC HDR (221-236). A texture of one holds its texels decoded, in a
// stored format the GPU samples exactly as an M4 Max samples the asked one (raw/pack/decode.metal,
// raw/test/decodeoracle.mm): its blocks are decoded on the GPU as they are uploaded (a blit's
// copyFromBuffer:toTexture:, replaceRegion through a blit of its own), and no copy of the blocks is kept.
#pragma once
#include <cstdint>
#include <cstring>

namespace raw {

// decode.metal's state, and astc_texel.h's form for ASTC
enum class DecodeState : uint8_t {
    Pvrtc,
    AstcUnorm8,
    AstcUnorm12,
    AstcHalf
};

struct DecodedFormat {
    uint32_t format, stored;
    uint8_t blockW, blockH, blockBytes;
    DecodeState state;
    bool opaque;  // PVRTC's RGB formats, which read alpha one
    // A never-written texel as the stored format holds it: the decode of a block of zeros, which Apple reads from a new
    // texture (PVRTC black, ASTC's error colour: magenta, an HDR NaN)
    uint32_t fresh[4];
};

constexpr uint32_t kRgba8Unorm = 70, kRgba8UnormSrgb = 71, kRgba16Float = 115, kRgba32Float = 125;
constexpr uint32_t kOne = 0x3f800000u, kMagenta8 = 0xffff00ffu, kNanHalves = 0xffffffffu;

// clang-format off
inline constexpr DecodedFormat kDecodedFormats[] = {
    {160, kRgba8Unorm, 8, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {161, kRgba8UnormSrgb, 8, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {162, kRgba8Unorm, 4, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {163, kRgba8UnormSrgb, 4, 4, 8, DecodeState::Pvrtc, true, {0xff000000u}},
    {164, kRgba8Unorm, 8, 4, 8, DecodeState::Pvrtc, false, {0}},
    {165, kRgba8UnormSrgb, 8, 4, 8, DecodeState::Pvrtc, false, {0}},
    {166, kRgba8Unorm, 4, 4, 8, DecodeState::Pvrtc, false, {0}},
    {167, kRgba8UnormSrgb, 4, 4, 8, DecodeState::Pvrtc, false, {0}},
    {185, kRgba8UnormSrgb, 4, 2, 16, DecodeState::AstcUnorm8, false, {kMagenta8}},
    {191, kRgba8UnormSrgb, 8, 4, 16, DecodeState::AstcUnorm8, false, {kMagenta8}},
    {203, kRgba32Float, 4, 2, 16, DecodeState::AstcUnorm12, false, {kOne, 0, kOne, kOne}},
    {209, kRgba32Float, 8, 4, 16, DecodeState::AstcUnorm12, false, {kOne, 0, kOne, kOne}},
    {221, kRgba16Float, 4, 2, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {222, kRgba16Float, 4, 4, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {223, kRgba16Float, 5, 4, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {224, kRgba16Float, 5, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {225, kRgba16Float, 6, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {226, kRgba16Float, 6, 6, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {227, kRgba16Float, 8, 4, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {228, kRgba16Float, 8, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {229, kRgba16Float, 8, 6, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {230, kRgba16Float, 8, 8, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {231, kRgba16Float, 10, 5, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {232, kRgba16Float, 10, 6, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {233, kRgba16Float, 10, 8, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {234, kRgba16Float, 10, 10, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {235, kRgba16Float, 12, 10, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
    {236, kRgba16Float, 12, 12, 16, DecodeState::AstcHalf, false, {kNanHalves, kNanHalves}},
};
// clang-format on

constexpr const DecodedFormat *decodedFormat(uint32_t format)
{
    for (const DecodedFormat &f : kDecodedFormats)
        if (f.format == format)
            return &f;
    return nullptr;
}

// The stored format's words as the integers a decode state writes them in: R32Uint, RGBA32Uint, RGBA16Uint
constexpr uint32_t decodeTargetFormat(DecodeState s)
{
    return s == DecodeState::AstcUnorm12 ? 123 : s == DecodeState::AstcHalf ? 113 : 53;
}

constexpr const char *decodeStateName(DecodeState s)
{
    switch (s) {
    case DecodeState::Pvrtc:
        return "raw.decode.decode_pvrtc_frag";
    case DecodeState::AstcUnorm8:
        return "raw.decode.decode_astc_unorm8_frag";
    case DecodeState::AstcUnorm12:
        return "raw.decode.decode_astc_unorm12_frag";
    case DecodeState::AstcHalf:
        return "raw.decode.decode_astc_half_frag";
    }
    return nullptr;
}

// Whether a view of a texture of `source` as `view` reads the image the texture holds: the format itself, or PVRTC's
// sRGB and linear forms of one block size and alpha, whose codes are the same. Any other view Metal makes of a decoded
// format (ASTC's sRGB and linear forms, LDR and HDR of one footprint) decodes the same blocks into other values.
constexpr bool decodedViewShared(uint32_t source, uint32_t view)
{
    const DecodedFormat *s = decodedFormat(source), *v = decodedFormat(view);
    if (!s && !v)
        return true;
    if (!s || !v)
        return false;
    return source == view || (s->state == DecodeState::Pvrtc && (source ^ 1) == view);
}

// decode.metal's buffer 0: its DecodeStep
struct DecodeStep {
    uint32_t x, y, blockW, blockH, blocksX, blocksY, rowBytes, form, first;
};

// A level's blocks along a side of `texels`: at least one
constexpr uint32_t decodedBlocks(uint32_t texels, uint32_t block)
{
    return texels ? (texels + block - 1) / block : 1;
}

// The step of a region whose first texel is (x, y) in a level of width x height, its blocks `rowBytes` apart from byte
// `first` of the bound buffer
inline DecodeStep decodeStep(const DecodedFormat &f, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                             uint32_t rowBytes, uint32_t first)
{
    // astc_texel.h kAstcUnorm8, kAstcUnorm12, kAstcHalf; PVRTC: 1 for a format without alpha
    const uint32_t form = f.state == DecodeState::Pvrtc         ? (f.opaque ? 1u : 0u)
                          : f.state == DecodeState::AstcUnorm8  ? 0u
                          : f.state == DecodeState::AstcUnorm12 ? 1u
                                                                : 2u;
    return {x,        y,    f.blockW, f.blockH, decodedBlocks(width, f.blockW), decodedBlocks(height, f.blockH),
            rowBytes, form, first};
}

// Block (x, y) of a PVRTC level's replaceRegion bytes: Morton order, y in the even bits, the longer side's extra bits
// on top (as an M4 Max reads them: test/raw_decode_vectors.h)
constexpr uint32_t pvrtcMortonBlock(uint32_t x, uint32_t y, uint32_t blocksX, uint32_t blocksY)
{
    uint32_t xLog2 = 0, yLog2 = 0;
    while ((1u << xLog2) < blocksX)
        xLog2++;
    while ((1u << yLog2) < blocksY)
        yLog2++;
    const uint32_t shared = xLog2 < yLog2 ? xLog2 : yLog2;
    uint32_t index = 0;
    for (uint32_t i = 0; i < shared; i++)
        index |= ((y >> i) & 1) << (2 * i) | ((x >> i) & 1) << (2 * i + 1);
    return index | (xLog2 > yLog2 ? x >> shared : y >> shared) << (2 * shared);
}

// A PVRTC level's replaceRegion bytes (Morton order) into rows, a blit's MTLBlitOptionRowLinearPVRTC order
inline void pvrtcRows(const uint8_t *morton, uint32_t blocksX, uint32_t blocksY, uint8_t *rows)
{
    for (uint32_t y = 0; y < blocksY; y++)
        for (uint32_t x = 0; x < blocksX; x++)
            memcpy(rows + 8 * (uint64_t(y) * blocksX + x),
                   morton + 8 * uint64_t(pvrtcMortonBlock(x, y, blocksX, blocksY)), 8);
}

}  // namespace raw
