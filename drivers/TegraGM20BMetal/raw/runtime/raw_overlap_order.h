// SPDX-License-Identifier: MIT
// The order an M4 Max stores a pass into a YCbCr target whose plane rows overlap (a pitch below its row), which decides the
// row each plane byte keeps (m2v-textures7 probes/overlap.mm and onepover.mm: 223344 bytes of 53 two-plane shapes and 40950
// of 10 one-plane ones, 8 runs each): tiles of 64x32 luma texels, each stored by 32-texel column, then row pair, the even
// row of a pair after the odd one (a 4:2:0 chroma row holds a pair's luma rows). Tiles are stored concurrently: runs of one
// shape differ wherever two tiles share a byte. The order here takes the tiles from the last tile row up, each row from its
// last tile back: every run of the shapes whose runs agree followed it (6 shapes), and the most runs of the rest (56%; the
// next best order 53%). pack/ycbcr_store_overlap.h draws each byte by it (raw_overlap_rows.h).
#pragma once

#include <cstdint>

#include "raw_reach.h"

namespace raw {

// The overlap states' fragment buffer 0 (pack/ycbcr_store_overlap.h OverlapRows)
struct OverlapRows {
    uint32_t pitch = 0, row = 0, rows = 0;  // the plane's bytes between rows, bytes a row stores, rows
    uint32_t element = 0;                   // bytes a texel of the source and of the target
    uint32_t base = 0;                      // the plane byte of the target's first texel; its rows `pitch` apart
    // a row's byte b lies at luma texel b * across / texelBytes, its row at row * down
    uint32_t texelBytes = 0, across = 1, down = 1;
    uint32_t masked = 0;  // a loaded pass: only rows in the 32x32 frame tiles its draws reach store (tiles)
    uint32_t kept = ~0u;  // a row's byte no store writes (an odd-width one-plane target's last Y1), ~0u: none
    uint32_t pairs = 0;   // a one-plane target's pairs from its luma and chroma images: 1 Y0 Cb Y1 Cr, 2 Cb Y0 Cr Y1
    TileMap tiles;        // each luma texel's frame tile, a turned target's in its frame
};

static_assert(sizeof(OverlapRows) == 76, "pack/ycbcr_store_overlap.h OverlapRows");

// Where the store of luma texel (x, y) lands in the order, later stores higher (pack/ycbcr_store_overlap.h overlapOrder)
inline uint32_t overlapOrder(uint32_t x, uint32_t y)
{
    const uint32_t tileRow = y / 32, tileColumn = x / 64;
    return (0xffffu - tileRow) << 14 | (0xffu - tileColumn) << 6 | (x % 64 / 32) << 5 | (y % 32 / 2) << 1 |
           ((y & 1) == 0 ? 1u : 0u);
}

// The row whose store of plane byte `a` lands last and that row's byte, as pack/ycbcr_store_overlap.h overlapWinner picks them;
// false where no store writes it (every row there kept or unreached). `reached(x, y)` names a reached luma texel.
template <typename Reached>
inline bool overlapWinner(const OverlapRows &s, uint32_t a, Reached reached, uint32_t &row, uint32_t &offset)
{
    const uint32_t last = a / s.pitch < s.rows - 1 ? a / s.pitch : s.rows - 1;
    const uint32_t first = a >= s.row ? (a - s.row) / s.pitch + 1 : 0;
    bool found = false;
    uint32_t best = 0;
    for (uint32_t r = first; r <= last; r++) {
        const uint32_t off = a - r * s.pitch, x = off * s.across / s.texelBytes, y = r * s.down;
        if (off == s.kept || (s.masked && !reached(x, y)))
            continue;
        if (const uint32_t order = overlapOrder(x, y); !found || order > best) {
            found = true;
            best = order;
            row = r;
            offset = off;
        }
    }
    return found;
}

// RGB10A8_2P_XR10 in two planes whose rows reach one another's bytes (raw_xr_pools.h xrPlanesCross): the overlap planes
// state's fragment buffer 0 (pack/ycbcr_store_overlap.h OverlapPlanes). Its target is the planes' span `pitch` bytes a row from
// `base`; each plane's first byte (from the span's), pitch, bytes a row and rows
struct OverlapPlanes {
    uint32_t base = 0, pitch = 0;
    uint32_t words = 0, wordPitch = 0, wordRow = 0, wordRows = 0;
    uint32_t alpha = 0, alphaPitch = 0, alphaRow = 0, alphaRows = 0;
};

static_assert(sizeof(OverlapPlanes) == 40, "pack/ycbcr_store_overlap.h OverlapPlanes");

// The texel whose store of span byte `a` lands last, of either plane, as an M4 Max's pass stores both: overlapOrder's key
// of each plane's texel, and between a word and an alpha byte of one key the texel further right, at one x the word (a
// 32x8 RGB10A8_2P_XR10 of words 128 bytes a row at 0 and alpha 32 at 512, cleared: alpha rows 0 to 3 keep word row 4,
// rows 4, 6 and 7 their own, row 5 word row 5's bytes up to its 11th); false where no row stores it. `byte`: the byte of
// a word texel, 4 for an alpha one
inline bool overlapPlanesWinner(const OverlapPlanes &s, uint32_t a, uint32_t &x, uint32_t &y, uint32_t &byte)
{
    bool found = false;
    uint32_t best = 0, bestX = 0;
    for (const bool word : {true, false}) {
        const uint32_t at = word ? s.words : s.alpha, pitch = word ? s.wordPitch : s.alphaPitch;
        const uint32_t row = word ? s.wordRow : s.alphaRow, rows = word ? s.wordRows : s.alphaRows;
        if (a < at)
            continue;
        const uint32_t off = a - at;
        const uint32_t last = off / pitch < rows - 1 ? off / pitch : rows - 1;
        const uint32_t first = off >= row ? (off - row) / pitch + 1 : 0;
        for (uint32_t r = first; r <= last; r++) {
            const uint32_t b = off - r * pitch, tx = word ? b / 4 : b, order = overlapOrder(tx, r);
            const uint32_t key = tx << 1 | (word ? 1u : 0u);
            if (!found || order > best || (order == best && key > bestX)) {
                found = true;
                best = order;
                bestX = key;
                x = tx;
                y = r;
                byte = word ? b % 4 : 4;
            }
        }
    }
    return found;
}

// A plane of `rows` rows of `rowBytes`, `pitch` apart, drawn `elementBytes` a texel
inline OverlapRows overlapRowsOf(uint32_t pitch, uint32_t rowBytes, uint32_t rows, uint32_t elementBytes,
                                 uint32_t texelBytes, uint32_t across, uint32_t down)
{
    OverlapRows s;
    s.pitch = pitch;
    s.row = rowBytes;
    s.rows = rows;
    s.element = elementBytes;
    s.texelBytes = texelBytes;
    s.across = across;
    s.down = down;
    return s;
}

// The pack's state drawing two planes that reach one another's bytes into their span, a byte a texel (R8Uint)
constexpr const char *kOverlapPlanesState = "raw.ycbcr_store.ycbcr_overlap_planes_frag";

// The pack's overlap state of a target format (pack/ycbcr_store_overlap.h), null for none
inline const char *overlapState(uint32_t format)
{
    switch (format) {
    case 10:  // R8Unorm: luma, and a one-plane target's pair bytes
        return "raw.ycbcr_store.ycbcr_overlap_r8_frag";
    case 20:  // R16Unorm
        return "raw.ycbcr_store.ycbcr_overlap_r16_frag";
    case 30:  // RG8Unorm
        return "raw.ycbcr_store.ycbcr_overlap_rg8_frag";
    case 53:  // R32Uint: a packed plane's words
        return "raw.ycbcr_store.ycbcr_overlap_r32ui_frag";
    case 60:  // RG16Unorm
        return "raw.ycbcr_store.ycbcr_overlap_rg16_frag";
    case 90:  // RGB10A2Unorm: a one-plane 4:4:4 target's words
        return "raw.ycbcr_store.ycbcr_overlap_rgb10a2_frag";
    default:
        return nullptr;
    }
}

}  // namespace raw
