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

// Two stores reaching one byte keep the later 2x2 quad of the frame, by quad row and then quad column, whichever plane
// it is: RGB10A8_2P_XR10 2x8 over words of rows 8 at 0 and alpha of rows 2 at 64, both stored 16 apart, alpha rows 0-3
// on word rows 4-7: the words keep the bytes turned 0 and 1 times, the alpha 2 and 3 times (an M4 Max's clears, draws
// and loaded passes alike, deterministic: m2v-textures13 probes/c4misc/RESULT.txt R3)
TEST(RawOverlapOrder, StoresThatMeetKeepTheLaterQuadOfTheFrame)
{
    raw::OverlapPlanes xr;
    xr.pitch = xr.wordPitch = xr.alphaPitch = 16;
    xr.wordRow = 8;
    xr.alphaRow = 2;
    xr.wordRows = xr.alphaRows = 8;
    xr.alpha = 64;
    for (uint8_t turn = 0; turn < 4; turn++) {
        xr.tiles = frameOfTurned(turn, 2, 8);
        for (uint32_t k = 0; k < 4; k++)
            for (uint32_t j = 0; j < 2; j++) {
                const uint32_t a = 64 + 16 * k + j;
                uint32_t x = 0, y = 0, byte = 0;
                ASSERT_TRUE(raw::overlapPlanesWinner(xr, a, x, y, byte)) << a;
                if (turn < 2)
                    EXPECT_EQ(std::make_tuple(x, y, byte), std::make_tuple(0u, 4 + k, j)) << "turn " << int(turn) << a;
                else
                    EXPECT_EQ(std::make_tuple(x, y, byte), std::make_tuple(j, k, 4u)) << "turn " << int(turn) << a;
            }
    }
}

// A pass that draws nothing stores by a clear's order: at a quarter turn the frame column further right lands later,
// inside a frame tile in every pass: 550 96x8 of words 192 bytes a row at 0 and alpha 96 at 1536, turned once, word row
// 7 keeping bytes 1568-1597 and 1696-1706 in all 84 passes of 12 processes, where a draw's order gives them to alpha rows
// 0 and 1 (an M4 Max: m2v-textures13 probes/c2clears case 61, probes/qclear/pick.txt)
TEST(RawOverlapOrder, AClearKeepsTheFrameColumnFurtherRightAtAQuarterTurn)
{
    raw::OverlapPlanes xr;
    xr.pitch = xr.wordPitch = 192;
    xr.wordRow = 384;
    xr.wordRows = xr.alphaRows = 8;
    xr.alpha = 1536;
    xr.alphaPitch = xr.alphaRow = 96;
    xr.tiles = frameOfTurned(1, 96, 8);
    xr.cleared = 1;
    for (const uint32_t a : {1568u, 1587u, 1597u, 1696u, 1706u}) {
        uint32_t x = 0, y = 0, byte = 0;
        ASSERT_TRUE(raw::overlapPlanesWinner(xr, a, x, y, byte)) << a;
        EXPECT_EQ(std::make_tuple(x, y, byte), std::make_tuple((a - 7 * 192) / 4, 7u, (a - 7 * 192) % 4)) << a;
    }
}
