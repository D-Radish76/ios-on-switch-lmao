// SPDX-License-Identifier: MIT
// The order an M4 Max stores a pass into a YCbCr target whose plane rows overlap (a pitch below its row), which decides the
// row each plane byte keeps (m2v-textures7 probes/overlap.mm and onepover.mm: 223344 bytes of 53 two-plane shapes and 40950
// of 10 one-plane ones, 8 runs each): tiles of 64x32 luma texels, each stored by 32-texel column, then row pair, the even
// row of a pair after the odd one (a 4:2:0 chroma row holds a pair's luma rows). Tiles are stored concurrently: runs of one
// shape differ wherever two tiles share a byte. The order here takes the tiles from the last tile row up, each row from its
// last tile back: every run of the shapes whose runs agree followed it (6 shapes), and the most runs of the rest (56%; the
// next best order 53%). A turned target is stored by its FRAME's tiles (the texture as the rasteriser draws it): in a
// frame tile by the frame's rows, with no miss in 1.46 M byte runs of 124 turned and unturned shapes; at a quarter turn the
// 64-wide frame column further left lands later, then the tile further down, the best fixed order of a race (62-70% of an
// M4 Max's bytes, 39-44% by the plane's tiles: m2v-textures12 probes/f3/RESULT.txt). pack/ycbcr_store_overlap.h draws
// each byte by it (raw_overlap_rows.h).
#pragma once

#include <algorithm>
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
    TileMap tiles;        // each luma texel's frame texel and tile, a turned target's in its frame
};

static_assert(sizeof(OverlapRows) == 76, "pack/ycbcr_store_overlap.h OverlapRows");

// Where the store of frame texel (x, y) lands inside its 64x32 tile: the right 32-texel half later, then the 2x2 quad's
// row, then its column, then the even row of a quad after the odd one (an M4 Max stores a pass by quads: the later quad
// row, then the later quad column, keeps a byte two quads reach: m2v-textures12 probes/f2/q1.txt R3, m2v-textures13
// probes/c4misc/RESULT.txt R3, at every turn)
inline uint32_t inTileOrder(uint32_t x, uint32_t y)
{
    return (x % 64 / 32) << 9 | (y % 32 / 2) << 5 | (x % 32 / 2) << 1 | ((y & 1) == 0 ? 1u : 0u);
}

// Where the store of frame texel (x, y) of an unturned or half-turned frame lands in the order, later stores higher
// (pack/ycbcr_store_overlap.h overlapOrder)
inline uint32_t overlapOrder(uint32_t x, uint32_t y)
{
    const uint32_t tileRow = y / 32, tileColumn = x / 64;
    return (0x3ffu - tileRow) << 18 | (0xffu - tileColumn) << 10 | inTileOrder(x, y);
}

// The frame texel `map` takes the top-left of a store of `across` x `down` plane texels from (x, y) to: the frame's first
// of them (a 4:2:0 chroma block's top-left frame texel)
inline void frameTexel(const TileMap &map, uint32_t x, uint32_t y, uint32_t across, uint32_t down, uint32_t &fx,
                       uint32_t &fy)
{
    const int32_t px = int32_t(x), py = int32_t(y), sx = int32_t(across) - 1, sy = int32_t(down) - 1;
    fx = uint32_t(map.ax * px + map.ay * py + map.cx + std::min(map.ax, 0) * sx + std::min(map.ay, 0) * sy);
    fy = uint32_t(map.bx * px + map.by * py + map.cy + std::min(map.bx, 0) * sx + std::min(map.by, 0) * sy);
}

// Where the store of frame texel (fx, fy) lands in the order of a frame `map` turns a quarter or not (ax 0: the plane's x
// runs down the frame)
inline uint32_t frameOrder(const TileMap &map, uint32_t fx, uint32_t fy)
{
    if (map.ax != 0)
        return overlapOrder(fx, fy);
    return (0x3ffu - fx / 64) << 22 | (fy / 32) << 10 | inTileOrder(fx, fy);
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
        uint32_t fx = 0, fy = 0;
        frameTexel(s.tiles, x, y, s.across, s.down, fx, fy);
        if (const uint32_t order = frameOrder(s.tiles, fx, fy); !found || order > best) {
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
// `base`; each plane's first byte (from the span's), pitch, bytes a row and rows; each plane texel's frame texel
struct OverlapPlanes {
    uint32_t base = 0, pitch = 0;
    uint32_t words = 0, wordPitch = 0, wordRow = 0, wordRows = 0;
    uint32_t alpha = 0, alphaPitch = 0, alphaRow = 0, alphaRows = 0;
    TileMap tiles;
};

static_assert(sizeof(OverlapPlanes) == 72, "pack/ycbcr_store_overlap.h OverlapPlanes");

// The texel whose store of span byte `a` lands last, of either plane, as an M4 Max's pass stores both: frameOrder's key
// of each plane texel's frame texel, and between a word and an alpha byte of one key the texel further right in the
// frame, at one x the word (a
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
            const uint32_t b = off - r * pitch, tx = word ? b / 4 : b;
            uint32_t fx = 0, fy = 0;
            frameTexel(s.tiles, tx, r, 1, 1, fx, fy);
            const uint32_t order = frameOrder(s.tiles, fx, fy), key = fx << 1 | (word ? 1u : 0u);
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
