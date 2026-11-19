// SPDX-License-Identifier: MIT
// The order of an M4 Max's stores where a target's plane rows reach the same bytes (raw_overlap_order.h): of a turned
// target by its frame's tiles and 2x2 quads
#include <gtest/gtest.h>

#include <tuple>
#include <utility>
#include <vector>

#include "../raw/runtime/raw_overlap_order.h"

namespace {

// A turned target of a plane's extent (raw_rotation.h frameOf)
struct Turned {
    uint8_t rotation;
    uint32_t width, height;
};

raw::TileMap frameOfTurned(uint8_t rotation, uint32_t width, uint32_t height)
{
    return raw::tileMap(raw::frameToPlane(raw::frameOf(Turned{rotation, width, height})), 0, 0);
}

}  // namespace

// A turned target's rows that overlap keep the store an M4 Max lands last inside a 32x32 tile of its FRAME: the later
// frame row pair, then the even frame row (m2v-textures12 probes/f3, bytes whose rows all lie in one frame tile, alike in
// 16 runs): 500 128x96 over a '420v' of rows 16 apart, its luma turned once and its chroma three times; 550 96x72 of
// words 96 bytes apart turned once, its alpha at 6912
TEST(RawOverlapOrder, ATurnedTargetsRowsKeepTheStoreAppleLandsLastInAFrameTile)
{
    const auto every = [](uint32_t, uint32_t) {
        return true;
    };
    raw::OverlapRows luma = raw::overlapRowsOf(16, 128, 96, 1, 1, 1, 1);
    luma.tiles = frameOfTurned(1, 128, 96);
    for (const auto &[a, r] : std::vector<std::pair<uint32_t, uint32_t>>{{16, 1}, {20, 1}, {1616, 95}, {1628, 95}}) {
        uint32_t row = 0, offset = 0;
        ASSERT_TRUE(raw::overlapWinner(luma, a, every, row, offset)) << a;
        EXPECT_EQ(row, r) << "luma byte " << a;
        EXPECT_EQ(offset, a - r * 16) << "luma byte " << a;
    }
    raw::OverlapRows chroma = raw::overlapRowsOf(16, 128, 48, 2, 2, 2, 2);
    chroma.tiles = frameOfTurned(3, 128, 96);
    for (const auto &[a, r] : std::vector<std::pair<uint32_t, uint32_t>>{{16, 0}, {28, 0}, {848, 46}, {860, 46}}) {
        uint32_t row = 0, offset = 0;
        ASSERT_TRUE(raw::overlapWinner(chroma, a, every, row, offset)) << a;
        EXPECT_EQ(row, r) << "chroma byte " << a;
        EXPECT_EQ(offset, a - r * 16) << "chroma byte " << a;
    }
    raw::OverlapPlanes xr;
    xr.pitch = xr.wordPitch = xr.alphaPitch = xr.alphaRow = 96;
    xr.wordRow = 384;
    xr.wordRows = xr.alphaRows = 72;
    xr.alpha = 6912;
    xr.tiles = frameOfTurned(1, 96, 72);
    for (const uint32_t a : {96u, 99u, 113u, 127u}) {
        uint32_t x = 0, y = 0, byte = 0;
        ASSERT_TRUE(raw::overlapPlanesWinner(xr, a, x, y, byte)) << a;
        EXPECT_EQ(std::make_tuple(x, y, byte), std::make_tuple((a - 96) / 4, 1u, a % 4)) << "word byte " << a;
    }
}
