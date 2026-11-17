// SPDX-License-Identifier: MIT
// BGR10_XR_sRGB's codes as Metal reads and writes them (raw/runtime/raw_xr_srgb.h, the model of pack/xr.metal
// xr_bgr10_srgb_frag and pack/fmt.metal fmt_store_bgr10_xr_srgb_frag over Apple's tables): its reads against an M3 Max's
// samples and the format's curve, its stores of what its loads read, Metal's writes of edge values, and its stand-in's clear.
#include "../raw/runtime/raw_format_standin.h"
#include "../raw/runtime/raw_xr_srgb.h"

#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

float asFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// A linear sample of a BGR10_XR_sRGB texture Metal took (m2v's fixture xr_samples.tsv, EV TXN-xrprobe4-m3 on an Apple M3
// Max): the fractions in 1/256, the red, green and blue codes of the taps (i, j), (i + 1, j), (i, j + 1), (i + 1, j + 1),
// and the result's lanes as float bits, or as half bits in a half read
struct SrgbSample {
    uint32_t kx, ky, taps[12], bits[3];
};

const SrgbSample kSrgbFloatReads[] = {
    {96, 167, {110, 958, 582, 703, 754, 537, 804, 743, 274, 219, 22, 182}, {0x3e6ea000, 0x3ed6e000, 0xbc300000}},
    {66, 47, {107, 951, 188, 484, 673, 635, 311, 522, 894, 961, 867, 1015}, {0xbdb4c000, 0x3f617000, 0x3e3b2000}},
    {1, 141, {947, 968, 581, 1019, 739, 453, 702, 440, 870, 791, 104, 1000}, {0x3f414800, 0x3f1dc000, 0x3f0ce000}},
    {157, 130, {265, 918, 458, 440, 328, 230, 668, 333, 582, 944, 201, 11}, {0x3ede0000, 0x3e312000, 0xbe17c000}},
    {187, 120, {214, 630, 34, 683, 33, 419, 977, 662, 1007, 950, 97, 696}, {0x3f379000, 0xbe4e0000, 0x3e824000}},
    {242, 7, {42, 73, 65, 317, 102, 460, 241, 608, 857, 833, 572, 794}, {0xbc890000, 0xbe84c000, 0x3c860000}},
    {77, 214, {48, 423, 690, 481, 351, 146, 838, 826, 589, 191, 910, 742}, {0x3ec07000, 0x3f316000, 0x3e608000}},
    {26, 243, {121, 238, 862, 506, 889, 850, 959, 71, 516, 928, 517, 862}, {0x3f9c8400, 0xbe8e7000, 0x3e316000}},
    {11, 80, {795, 637, 445, 687, 698, 343, 676, 674, 898, 539, 810, 18}, {0x3f000800, 0x3e77e000, 0x3e9d0000}},
    {173, 248, {476, 889, 190, 193, 743, 959, 646, 627, 85, 73, 889, 53}, {0xbe168000, 0x3f386000, 0xbea21000}},
    {12, 224, {982, 945, 463, 607, 102, 265, 300, 245, 252, 791, 178, 205}, {0x3e35e000, 0x3db98000, 0xbd420000}},
    {180, 181, {700, 843, 540, 134, 10, 68, 149, 340, 820, 0, 17, 650}, {0xbe9fe000, 0xbe8b8000, 0x3e4cc000}},
};

const SrgbSample kSrgbHalfReads[] = {
    {248, 180, {394, 215, 850, 607, 601, 324, 994, 172, 806, 1007, 566, 399}, {0x3c9f, 0x2f18, 0x2510}},
    {118, 16, {783, 354, 934, 621, 694, 711, 832, 227, 633, 309, 259, 439}, {0x364d, 0x3054, 0x3a23}},
    {161, 76, {958, 578, 472, 813, 841, 905, 8, 237, 600, 242, 866, 286}, {0x3897, 0x3843, 0x37b5}},
    {198, 113, {681, 536, 1008, 215, 541, 1023, 125, 661, 496, 602, 70, 862}, {0x2770, 0xaa04, 0x3ce4}},
    {230, 225, {916, 55, 914, 713, 796, 295, 25, 571, 734, 139, 89, 783}, {0xb085, 0xb11c, 0x3805}},
    {123, 43, {509, 642, 153, 1023, 479, 43, 371, 872, 826, 421, 829, 451}, {0x3987, 0x33d4, 0xb183}},
    {6, 3, {619, 73, 284, 250, 118, 723, 150, 969, 461, 817, 2, 307}, {0x3172, 0xb4f0, 0xa570}},
    {160, 42, {78, 183, 646, 764, 82, 828, 670, 699, 409, 67, 594, 369}, {0x30d3, 0xb155, 0x373f}},
};

double halfValue(uint32_t bits)
{
    _Float16 h;
    const uint16_t b = uint16_t(bits);
    memcpy(&h, &b, 2);
    return double(h);
}

// Lane `lane` of sample `s`, `metal`, against the conversion's reads of its taps weighted as Metal weighs them
void expectSample(const SrgbSample &s, uint32_t lane, double metal)
{
    const double fx = s.kx / 256.0, fy = s.ky / 256.0;
    const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    double v = 0;
    for (uint32_t k = 0; k < 4; k++)
        v += w[k] * raw::bgr10XrSrgbValue(s.taps[3 * k + lane]);
    EXPECT_NEAR(v, metal, std::ldexp(1.0, -11)) << s.kx << " " << s.ky << " lane " << lane;
}

// The stand-in a draw samples holds each texel as the conversion reads it: weighted as a sample's taps, Metal's samples,
// within a half's rounding at their magnitudes (Metal filters exactly, the stand-in's texels are halves)
TEST(Bgr10XrSrgb, ItsReadsFilterToMetalsSamples)
{
    for (const SrgbSample &s : kSrgbFloatReads)
        for (uint32_t lane = 0; lane < 3; lane++)
            expectSample(s, lane, asFloat(s.bits[lane]));
    for (const SrgbSample &s : kSrgbHalfReads)
        for (uint32_t lane = 0; lane < 3; lane++)
            expectSample(s, lane, halfValue(s.bits[lane]));
}

// Each code reads the value BGR10_XR_sRGB defines, the sRGB curve over (code - 384) / 510 mirrored about 0, to the
// table's 2^-14 and a half's rounding below 2; a step midway between two halves reads the one away from zero, as Metal reads
// every such tie (m2v lower/arith/extended_range.rs decode, EV TXN-xrprobe-m3): steps 2049 and 13476 of 8192
TEST(Bgr10XrSrgb, ACodeReadsItsFormatsValue)
{
    for (uint32_t code = 0; code < 1024; code++) {
        const double x = std::fabs((double(code) - 384.0) / 510.0);
        const double linear = x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
        EXPECT_NEAR(raw::bgr10XrSrgbValue(code), code < 384 ? -linear : linear,
                    std::ldexp(1.0, -11) + std::ldexp(1.0, -14))
            << code;
    }
    EXPECT_EQ(raw::bgr10XrSrgbValue(384 + 274), 2050.0f / 8192.0f);
    EXPECT_EQ(raw::bgr10XrSrgbValue(384 - 274), -2050.0f / 8192.0f);
    EXPECT_EQ(raw::bgr10XrSrgbValue(384 + 634), 13480.0f / 8192.0f);
}

// A pass that loads a texel and draws nothing over it stores its codes as they were: the store state's encode of the load's
// halves, every code of every channel, bits 30 and 31 set as Metal's write sets them
TEST(Bgr10XrSrgb, AStoreKeepsTheCodesALoadRead)
{
    for (uint32_t code = 0; code < 1024; code++) {
        const uint32_t g = 1023 - code, b = code * 7 % 1024;
        const float v[4] = {raw::bgr10XrSrgbValue(code), raw::bgr10XrSrgbValue(g), raw::bgr10XrSrgbValue(b), 1.0f};
        EXPECT_EQ(raw::bgr10XrSrgbWord(v), 0xC0000000u | code << 20 | g << 10 | b) << code;
    }
}

// Metal's write of a NaN stores the zero code and of a value past the codes the nearest end; its tile holds a clear as the
// colour's nearest halves and BGR10's alpha of 1 (m2v ExtendedRange::code, clear_tile_value)
TEST(Bgr10XrSrgb, ItStoresMetalsWritesAndHoldsAClearInHalves)
{
    const float nan = std::nanf(""), inf = std::numeric_limits<float>::infinity();
    const float edges[4] = {nan, inf, -inf, 1.0f};
    EXPECT_EQ(raw::bgr10XrSrgbWord(edges), 0xC0000000u | 384u << 20 | 1023u << 10 | 0u);
    const float clear[4] = {0.1f, -0.3f, 1.1f, 0.6f};
    float held[4];
    raw::formatStandInClear(raw::kBgr10XrSrgb, clear, held, true);
    for (int k = 0; k < 3; k++)
        EXPECT_EQ(held[k], float(_Float16(clear[k]))) << k;
    EXPECT_EQ(held[3], 1.0f);
}

}  // namespace
