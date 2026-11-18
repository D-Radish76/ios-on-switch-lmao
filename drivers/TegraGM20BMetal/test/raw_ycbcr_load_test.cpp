// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_load.h: a pass that loads a two-plane YCbCr target. The tile value of each read (the nearest half, a
// tie up), and the planes after a loaded pass's draw by the raw path's route (load state, draws, tile-masked store) against
// an M4 Max's own passes: the hashes raw/test/ycbcrloadoracle.mm golden printed of Metal's planes (that oracle holds the
// route's states, run on the Mac's GPU, to every byte of 1456 passes over every format and surface).
#include "raw_ycbcr_load_cases.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

const uint16_t kKnots[513] = {
#include "../raw/pack/srgb_knots.h"
};

// The half nearest num / den by exhaustive search over the halves in [0, 1], a tie the larger
double nearestHalf(uint32_t num, uint32_t den)
{
    double best = 0, bestOff = 2;
    for (uint32_t bits = 0; bits <= 0x3c00; bits++) {
        _Float16 h;
        const uint16_t b = uint16_t(bits);
        memcpy(&h, &b, 2);
        const double off = std::fabs(double(h) * den - double(num));
        if (off < bestOff || (off == bestOff && double(h) > best)) {
            best = double(h);
            bestOff = off;
        }
    }
    return best;
}

TEST(RawYcbcrLoad, TheTileHoldsEachReadAsTheNearestHalfATieUp)
{
    for (const uint32_t den : {255u, 1023u, 2048u, 8192u})
        for (uint32_t num = 0; num <= den; num++)
            ASSERT_EQ(double(raw::ycbcrLoadHalf(num, den)), nearestHalf(num, den)) << num << " / " << den;
    // an M4 Max's fetches of YCBCR8_420_2P_sRGB's steps (ties) beside their reads
    EXPECT_EQ(raw::ycbcrLoadHalf(7554, 8192), 7556.0f / 8192);
    EXPECT_EQ(raw::ycbcrLoadHalf(4106, 8192), 4108.0f / 8192);
    EXPECT_EQ(raw::ycbcrLoadHalf(2441, 8192), 2442.0f / 8192);
    EXPECT_EQ(raw::ycbcrLoadHalf(3549, 8192), 0.433349609f);
}

struct Golden {
    ycbcrload::Case c;
    uint64_t metal;
};

// raw/test/ycbcrloadoracle.mm golden on an M4 Max under macOS 27.2: Metal's planes after kGoldenArm
const Golden kGolden[] = {
    {{540, ycbcrload::fourcc("420v"), nullptr}, 0x11baa8cf6c3e05b9ull},
    {{500, ycbcrload::fourcc("420f"), "ITU_R_709_2"}, 0x93101cdce779635dull},
    {{520, ycbcrload::fourcc("420v"), nullptr}, 0x1b55873bc381acdaull},
    {{545, ycbcrload::fourcc("x444"), nullptr}, 0x763b349863a7149full},
    {{503, ycbcrload::fourcc("444f"), nullptr}, 0xa37b52d89dc171b7ull},
    {{541, ycbcrload::fourcc("422v"), nullptr}, 0x96cf2d54459e13a9ull},
    {{543, ycbcrload::fourcc("xf20"), nullptr}, 0xf9c567a96d4fd656ull},
    {{525, ycbcrload::fourcc("x420"), "ITU_R_2020"}, 0x329f4c9f31b04da9ull},
    {{527, ycbcrload::fourcc("xf44"), nullptr}, 0x8d5804ad7f5b74cdull},
    {{506, ycbcrload::fourcc("x422"), "ITU_R_709_2"}, 0x31e83eb45b2f622bull},
};

// A surface's facts of `c` at w x h: rows 64-byte aligned, plane 1 after plane 0
raw::YcbcrFacts factsOf(const ycbcrload::Case &c, uint32_t w, uint32_t h)
{
    const bool tenBit = (c.fourcc >> 24) == 'x' || (c.fourcc >> 24) == 'p';
    const uint32_t pitch = (w * (tenBit ? 2 : 1) * 2 + 63) / 64 * 64;
    raw::YcbcrFacts f;
    f.pixelFormat = c.format;
    f.fourcc = c.fourcc;
    f.planes = 2;
    f.width = f.surfaceWidth = w;
    f.height = f.surfaceHeight = h;
    f.offset[1] = uint64_t(pitch) * h;
    f.pitch[0] = f.pitch[1] = pitch;
    f.allocBytes = 2 * f.offset[1];
    f.simple = true;
    f.target = true;
    f.matrix = c.matrix;
    return f;
}

struct Pass {
    raw::YcbcrPlan plan;
    ycbcrload::Layout layout;
    ycbcrload::Planes before;
    std::vector<float> values;
};

Pass goldenPass(const ycbcrload::Case &c)
{
    const ycbcrload::Size size = ycbcrload::sizes(true)[0];
    Pass p;
    EXPECT_EQ(raw::planYcbcr(factsOf(c, size[0], size[1]), p.plan), nullptr);
    p.layout = ycbcrload::layoutOf(p.plan, size[0], size[1]);
    p.before = ycbcrload::randomPlanes(p.layout, c.format * 131 + size[0]);
    p.values = ycbcrload::drawnValues(size[0] * size[1], c.format * 7 + size[1]);
    return p;
}

TEST(RawYcbcrLoad, ALoadedPassStoresTheTilesItsDrawReachesAsMetalDoes)
{
    for (const Golden &g : kGolden) {
        const Pass p = goldenPass(g.c);
        const ycbcrload::Planes after =
            ycbcrload::modelPass(g.c, p.plan, p.layout, p.before, p.values, ycbcrload::kGoldenArm, kKnots);
        EXPECT_EQ(ycbcrload::hash(after), g.metal) << g.c.format;
    }
}

TEST(RawYcbcrLoad, StoringEveryTileOfALoadedPassIsNotMetals)
{
    for (const Golden &g : kGolden) {
        const Pass p = goldenPass(g.c);
        ycbcrload::Arm every = ycbcrload::kGoldenArm;
        every.second = true;
        const ycbcrload::Planes after = ycbcrload::modelPass(g.c, p.plan, p.layout, p.before, p.values, every, kKnots);
        if (g.c.format != 545)  // 4:4:4 codes come back as they were loaded
            EXPECT_NE(ycbcrload::hash(after), g.metal) << g.c.format;
    }
}

TEST(RawYcbcrLoad, ALoadedPassWithoutADrawStoresNothing)
{
    for (const Golden &g : kGolden) {
        const Pass p = goldenPass(g.c);
        ycbcrload::Arm none;
        none.draws = false;
        EXPECT_EQ(ycbcrload::modelPass(g.c, p.plan, p.layout, p.before, p.values, none, kKnots), p.before);
    }
}

TEST(RawYcbcrLoad, EveryLoadedFormatHasALoadStateAndAMaskedStore)
{
    for (const ycbcrload::Case &c : ycbcrload::cases()) {
        raw::YcbcrPlan plan;
        const raw::YcbcrFacts f = factsOf(c, 64, 32);
        if (raw::planYcbcr(f, plan))
            continue;
        EXPECT_NE(raw::ycbcrLoadState(plan.output), nullptr) << c.format;
        EXPECT_NE(raw::ycbcrMaskedStoreState(false, plan.store), nullptr) << c.format;
        EXPECT_NE(raw::ycbcrMaskedStoreState(true, plan.store), nullptr) << c.format;
    }
}

// An unpacked converted or sRGB target under a P3 matrix (planYcbcr takes every one) loads through a state of its own and
// stores through tile-masked states; a codes format (RGB*_2P) reads its codes under any matrix
TEST(RawYcbcrLoad, AP3TargetHasALoadStateAndAMaskedStore)
{
    uint32_t targets = 0;
    for (ycbcrload::Case c : ycbcrload::cases()) {
        if (c.matrix)
            continue;
        raw::YcbcrPlan plain;
        if (raw::planYcbcr(factsOf(c, 64, 32), plain) ||
            (plain.output != raw::YcbcrConverted && plain.output != raw::YcbcrSrgb))
            continue;
        c.matrix = "P3_D65";
        raw::YcbcrPlan plan;
        if (raw::planYcbcr(factsOf(c, 64, 32), plan))
            continue;
        targets++;
        ASSERT_TRUE(raw::ycbcrLumaFirst(plan.output)) << c.format;
        EXPECT_STREQ(raw::ycbcrLoadState(plan.output), "raw.ycbcr.ycbcr_load_luma_first_frag") << c.format;
        EXPECT_NE(raw::ycbcrMaskedStoreState(false, plan.store), nullptr) << c.format;
        EXPECT_NE(raw::ycbcrMaskedStoreState(true, plan.store), nullptr) << c.format;
    }
    EXPECT_GT(targets, 0u);
}

// Under a P3 matrix an M4 Max stores an sRGB 4:4:4 target as one code everywhere, whatever is drawn (128 at 8 bits, 384
// at 10: raw/test/p3storeoracle.mm)
TEST(RawYcbcrLoad, AnSrgbFourFourFourTargetUnderP3StoresOneCode)
{
    for (const ycbcrload::Case &c : {ycbcrload::Case{523, ycbcrload::fourcc("444v"), "P3_D65"},
                                     ycbcrload::Case{527, ycbcrload::fourcc("x444"), "DCI_P3"}}) {
        raw::YcbcrPlan plan;
        ASSERT_EQ(raw::planYcbcr(factsOf(c, 64, 32), plan), nullptr) << c.format;
        const int32_t code = c.format == 523 ? 128 : 384;
        for (int k = 0; k < 3; k++) {
            EXPECT_EQ(plan.store.offset[k], code) << c.format;
            for (int ch = 0; ch < 3; ch++)
                EXPECT_EQ(plan.store.row[k][ch], 0) << c.format;
        }
        EXPECT_NE(raw::ycbcrMaskedStoreState(false, plan.store), nullptr) << c.format;
        EXPECT_STREQ(raw::ycbcrLoadState(plan.output), "raw.ycbcr.ycbcr_load_luma_first_frag") << c.format;
    }
}

// and a 4:2:0 or 4:2:2 one as its codes format, each texel's code k first taken to the low bits of the sRGB encode E[k]
// (green 0.5: code 128, E 1078, stored 54): one block of four texels as an M4 Max stored it (raw/test/p3storeoracle.mm)
TEST(RawYcbcrLoad, AnSubsampledSrgbTargetUnderP3StoresEachCodeEncoded)
{
    static uint16_t encode[raw::kYcbcrSrgbSteps + 1];
    raw::ycbcrSrgbEncodeTable(kKnots, encode);
    const float rgba[16] = {1.0f, 0.5f, 0.25f, 1, 0, 1.0f, 0.75f, 1, 0.5f, 0, 1.0f, 1, 0.25f, 0.75f, 0, 1};

    struct Stored {
        ycbcrload::Case c;
        uint16_t luma[4], chroma[2];
    };

    const Stored stored[] = {{{520, ycbcrload::fourcc("420v"), "P3_D65"}, {54, 48, 0, 76}, {77, 72}},
                             {{525, ycbcrload::fourcc("x420"), "P3_D65"}, {224, 109, 0, 724}, {349, 224}}};
    for (const Stored &s : stored) {
        raw::YcbcrPlan plan;
        ASSERT_EQ(raw::planYcbcr(factsOf(s.c, 2, 2), plan), nullptr) << s.c.format;
        uint16_t luma[4], chroma[2];
        ASSERT_TRUE(raw::ycbcrStoreDrawn(plan.store, encode, rgba, 2, 2, luma, chroma));
        for (int i = 0; i < 4; i++)
            EXPECT_EQ(luma[i], s.luma[i]) << s.c.format << " texel " << i;
        EXPECT_EQ(chroma[0], s.chroma[0]) << s.c.format;
        EXPECT_EQ(chroma[1], s.chroma[1]) << s.c.format;
    }
}

// A packed one stores its unpacked twin's codes three to a word, as every packed target does (Metal's passes into one
// fault on an M4 Max)
TEST(RawYcbcrLoad, APackedSrgbTargetUnderP3StoresItsTwinsEncodedCodes)
{
    const ycbcrload::Case pairs[][2] = {
        {{528, ycbcrload::fourcc("pf20"), "P3_D65"}, {525, ycbcrload::fourcc("xf20"), "P3_D65"}},
        {{529, ycbcrload::fourcc("p422"), "DCI_P3"}, {526, ycbcrload::fourcc("x422"), "DCI_P3"}}};
    for (const auto &pair : pairs) {
        raw::YcbcrPlan packed, twin;
        ASSERT_EQ(raw::planYcbcr(factsOf(pair[0], 48, 32), packed), nullptr) << pair[0].format;
        ASSERT_EQ(raw::planYcbcr(factsOf(pair[1], 48, 32), twin), nullptr) << pair[1].format;
        const raw::YcbcrStoreShape words = raw::ycbcrPackedShape(twin.store);
        EXPECT_EQ(twin.store.srgb, 1u) << pair[1].format;
        EXPECT_EQ(memcmp(&packed.store, &words, sizeof words), 0) << pair[0].format;
    }
}

// The raw path's route of a pass drawing floats over the whole frame (no texel loaded), against the hash of Metal's planes
// after the same pass on an M4 Max (raw/test/p3storeoracle.mm golden; both ranges and both P3 matrices store alike)
TEST(RawYcbcrLoad, AnSubsampledSrgbTargetUnderP3StoresAWholeFrameAsMetalDoes)
{
    const Golden golden[] = {{{520, ycbcrload::fourcc("420v"), "P3_D65"}, 0x9d81a93082ec22adull},
                             {{522, ycbcrload::fourcc("422f"), "DCI_P3"}, 0x09d405b060535d24ull},
                             {{525, ycbcrload::fourcc("xf20"), "P3_D65"}, 0x941d8115c2c4b0c6ull},
                             {{526, ycbcrload::fourcc("x422"), "DCI_P3"}, 0x6ba10df57f1f4da0ull}};
    for (const Golden &g : golden) {
        const ycbcrload::Size size = ycbcrload::sizes(true)[0];
        raw::YcbcrPlan plan;
        ASSERT_EQ(raw::planYcbcr(factsOf(g.c, size[0], size[1]), plan), nullptr) << g.c.format;
        const Pass p = goldenPass(g.c);
        const ycbcrload::Arm whole = {0, 0, size[0], size[1], true, false, "whole"};
        EXPECT_EQ(ycbcrload::hash(ycbcrload::modelPass(g.c, p.plan, p.layout, p.before, p.values, whole, kKnots)),
                  g.metal)
            << g.c.format;
    }
}

// A 4:4:4 converted target under P3 stores as its codes format (raw/test/p3storeoracle.mm: 503 and 507, every code)
TEST(RawYcbcrLoad, AFourFourFourConvertedTargetUnderP3StoresItsCodes)
{
    raw::YcbcrPlan plan;
    ASSERT_EQ(raw::planYcbcr(factsOf({507, ycbcrload::fourcc("x444"), "P3_D65"}, 64, 32), plan), nullptr);
    EXPECT_EQ(plan.store.converted, 0u);
    EXPECT_EQ(plan.store.max, 1023u);
    EXPECT_EQ(plan.store.across, 0u);
    EXPECT_EQ(plan.store.down, 0u);
}

// Its Load holds the codes in the read's order, red Y, green Cb, blue Cr, where a codes format's holds red Cr, green Y,
// blue Cb, each the nearest half of code / max
TEST(RawYcbcrLoad, AP3TargetsLoadHoldsLumaThenCbThenCr)
{
    uint32_t shape[raw::kYcbcrShapeWords] = {2, 2};
    shape[12] = 255;
    shape[13] = 3;
    shape[14] = 1;
    const uint16_t luma[4] = {16, 100, 200, 235}, chroma[2] = {60, 190};
    const raw::YcbcrPlaneCodes codes = {luma, chroma, 1};
    float lumaFirst[4], codesOrder[4];
    raw::ycbcrLoadTexel(raw::YcbcrLumaFirst8, shape, codes, nullptr, 1, 0, lumaFirst);
    raw::ycbcrLoadTexel(raw::YcbcrCodes8, shape, codes, nullptr, 1, 0, codesOrder);
    EXPECT_EQ(lumaFirst[0], raw::ycbcrLoadHalf(100, 255));
    EXPECT_EQ(lumaFirst[1], raw::ycbcrLoadHalf(60, 255));
    EXPECT_EQ(lumaFirst[2], raw::ycbcrLoadHalf(190, 255));
    EXPECT_EQ(lumaFirst[3], 1.0f);
    EXPECT_EQ(lumaFirst[0], codesOrder[1]);
    EXPECT_EQ(lumaFirst[1], codesOrder[2]);
    EXPECT_EQ(lumaFirst[2], codesOrder[0]);
}

// A Load of a 420v target's planes as the M4 Max's tile held them (raw/test/ycbcrloadoracle.mm's probe of 16 x 8 texels):
// red Cr, green luma, blue Cb, the chroma between blocks rounded once, half up
TEST(RawYcbcrLoad, ALoadHoldsTheChromaBetweenBlocksRoundedOnce)
{
    constexpr uint32_t W = 16, H = 8;
    std::vector<uint16_t> luma(W * H), chroma(W * H / 2);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            luma[y * W + x] = uint16_t(16 + (x * 13 + y * 29) % 220);
    for (uint32_t y = 0; y < H / 2; y++)
        for (uint32_t x = 0; x < W; x++)
            chroma[y * W + x] = uint16_t(16 + (x * 37 + y * 53) % 224);
    std::vector<float> tile(W * H * 4);
    raw::ycbcrLoaded({255, true}, luma.data(), chroma.data(), W, H, tile.data());
    const auto code = [&](uint32_t x, uint32_t y, int c) {
        return std::lround(tile[4 * (y * W + x) + c] * 255);
    };
    EXPECT_EQ(code(0, 0, 0), 53);  // an edge texel: its block's own Cr
    EXPECT_EQ(code(1, 0, 0), 72);  // 71.5 across, half up
    EXPECT_EQ(code(2, 0, 0), 109);
    EXPECT_EQ(code(5, 0, 0), 164);
    EXPECT_EQ(code(0, 1, 0), 66);
    EXPECT_EQ(code(0, 1, 2), 29);
    EXPECT_EQ(code(1, 1, 0), 85);  // 84.75 over four blocks
    EXPECT_EQ(code(15, 0, 0), 123);
    EXPECT_EQ(code(3, 1, 1), 84);
    EXPECT_EQ(tile[3], 1.0f);
    EXPECT_EQ(tile[4 * 17 + 0], float(_Float16(85.0f / 255)));
}

// The same 420v planes loaded and drawn over through write mask green: the chroma the store averages is the loaded mix, so
// a block reloaded and stored moves (block 0's Cr 53 becomes the mean of 53, 72, 66 and 85 rounded up, 69), and the luma
// is the drawn green's code
TEST(RawYcbcrLoad, ALumaOnlyDrawOverALoadStoresTheLoadedChromasMean)
{
    constexpr uint32_t W = 16, H = 8;
    std::vector<uint16_t> luma(W * H), chroma(W * H / 2), lumaOut(W * H), chromaOut(W * H / 2);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            luma[y * W + x] = uint16_t(16 + (x * 13 + y * 29) % 220);
    for (uint32_t y = 0; y < H / 2; y++)
        for (uint32_t x = 0; x < W; x++)
            chroma[y * W + x] = uint16_t(16 + (x * 37 + y * 53) % 224);
    std::vector<float> green(W * H);
    for (uint32_t x = 0; x < W; x++)
        for (uint32_t y = 0; y < H; y++)
            green[y * W + x] = (float(x) + 0.5f) * 0.0123f;
    ASSERT_TRUE(raw::ycbcrStoreOverLoad({255, true}, luma.data(), chroma.data(), green.data(), W, H, lumaOut.data(),
                                        chromaOut.data()));
    EXPECT_EQ(chroma[1], 53u);
    EXPECT_EQ(chromaOut[1], 69u);
    EXPECT_EQ(lumaOut[0], uint16_t(std::floor(double(_Float16(0.5f * 0.0123f)) * 255 + 0.5)));
    EXPECT_EQ(lumaOut[W * H - 1], uint16_t(std::floor(double(_Float16(15.5f * 0.0123f)) * 255 + 0.5)));
    EXPECT_FALSE(raw::ycbcrStoreOverLoad({255, true}, luma.data(), chroma.data(), green.data(), W - 1, H,
                                         lumaOut.data(), chromaOut.data()));
}

}  // namespace
