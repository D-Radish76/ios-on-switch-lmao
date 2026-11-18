// SPDX-License-Identifier: MIT
// What Metal's blits move of a texture held through planes (raw_blit_planes.h), as an M4 Max under macOS 27.2.1 moves it
// with validation off: plane 0's bytes, planeUnit of them a texel
#include "../raw/runtime/raw_blit_planes.h"

#include <gtest/gtest.h>

namespace {

struct Planes {
    raw::Import import;
    raw::Texture luma, t;

    Planes()
    {
        import.id = 7;
        luma.import = &import;
        luma.va = 0x10000;
        luma.pitch = 64;
        luma.height = 8;
        t.width = 24;
        t.height = 8;
    }
};

TEST(BlitPlanes, EachKindCountsPlaneZeroInItsUnit)
{
    Planes eight, ten, packed, one, xr, planar;
    eight.luma.format = 10;  // R8Unorm codes
    ten.luma.format = 20;    // R16Unorm codes
    packed.luma.format = 53;  // packed 10-bit words
    for (Planes *p : {&eight, &ten, &packed}) {
        p->t.ycbcr = 1;
        p->t.luma = &p->luma;
    }
    one.t.ycbcr = 1;
    one.t.onePlane = true;
    one.t.import = &one.import;
    one.t.planes = {0x20000, 0, 128, 0};
    xr.t.xrPlanes = true;
    xr.t.luma = &xr.luma;
    planar.t.planar = true;
    planar.t.import = &planar.import;
    planar.t.planes = {0x30000, 0x40000, 32, 64};
    EXPECT_EQ(raw::planeUnit(eight.t), 1u);
    EXPECT_EQ(raw::planeUnit(ten.t), 2u);
    EXPECT_EQ(raw::planeUnit(packed.t), 1u);
    EXPECT_EQ(raw::planeUnit(one.t), 2u);
    EXPECT_EQ(raw::planeUnit(xr.t), 4u);
    EXPECT_EQ(raw::planeUnit(planar.t), 1u);
    EXPECT_EQ(raw::planeUnit(raw::Texture()), 0u);

    raw::Texture out;
    ASSERT_TRUE(raw::planeBytes(packed.t, 1, out));
    EXPECT_EQ(out.va, 0x10000u);
    EXPECT_EQ(out.pitch, 64u);
    EXPECT_EQ(out.width, 64u);
    EXPECT_EQ(out.height, 8u);
    EXPECT_EQ(out.format, 10u);
    EXPECT_FALSE(out.blockLinear);
    ASSERT_TRUE(raw::planeBytes(one.t, 2, out));
    EXPECT_EQ(out.va, 0x20000u);
    EXPECT_EQ(out.width, 64u);
    EXPECT_EQ(out.height, 8u);
    EXPECT_EQ(out.bpp, 2u);
    ASSERT_TRUE(raw::planeBytes(xr.t, 16, out));  // counted in a plain RGBA32 texture's texel
    EXPECT_EQ(out.width, 4u);
    EXPECT_EQ(out.format, 123u);
    EXPECT_EQ(out.storage.bytes, 64u * 7 + 64u);
    EXPECT_FALSE(raw::planeBytes(xr.t, 3, out));
    EXPECT_FALSE(raw::planeBytes(xr.t, 128, out));
    xr.luma.blockLinear = true;
    EXPECT_FALSE(raw::planeBytes(xr.t, 4, out));
}

// Plane 1 by its own geometry: the pairs a region's texels fall in, a packed plane's whole words of them, never past its
// rows; RGB10A8_2P_XR10's alpha bytes texel for texel
TEST(BlitPlanes, PlaneOneSpansThePairsTheRegionCovers)
{
    raw::Import import;
    import.id = 7;
    raw::Texture luma, chroma, t;
    luma.import = chroma.import = &import;
    luma.va = 0x10000;
    luma.pitch = 64;
    luma.height = 8;
    luma.format = 10;
    chroma.va = 0x20000;
    chroma.pitch = 64;
    chroma.width = 8;  // 4:2:0 pairs of a 16x8 texture
    chroma.height = 4;
    chroma.bpp = 2;
    chroma.format = 30;
    t.ycbcr = 1;
    t.luma = &luma;
    t.chroma = &chroma;
    t.width = 16;
    t.height = 8;
    raw::Texture out;
    uint32_t bx, by, bw, bh;
    ASSERT_TRUE(raw::planeOneSpan(t, 2, 2, 8, 4, out, bx, by, bw, bh));
    EXPECT_EQ(out.va, 0x20000u);
    EXPECT_EQ(out.bpp, 1u);
    EXPECT_EQ(bx, 2u);
    EXPECT_EQ(bw, 8u);
    EXPECT_EQ(by, 1u);
    EXPECT_EQ(bh, 2u);
    ASSERT_TRUE(raw::planeOneSpan(t, 1, 1, 8, 7, out, bx, by, bw, bh));
    EXPECT_EQ(bx, 0u);
    EXPECT_EQ(bw, 10u);
    EXPECT_EQ(by, 0u);
    EXPECT_EQ(bh, 4u) << "never past the plane's rows";

    chroma.format = 53;  // packed: 16 codes a row, 6 words
    chroma.width = 6;
    t.shape[13] = 3;  // halved across and down
    ASSERT_TRUE(raw::planeOneSpan(t, 2, 2, 8, 4, out, bx, by, bw, bh));
    EXPECT_EQ(bx, 0u);
    EXPECT_EQ(bw, 16u);  // pairs 1..4: codes 2..9, words 0..3

    raw::Texture xr, alpha;
    alpha = luma;
    alpha.va = 0x30000;
    alpha.width = 16;
    alpha.bpp = 1;
    xr.xrPlanes = true;
    xr.luma = &luma;
    xr.chroma = &alpha;
    xr.width = 16;
    xr.height = 8;
    ASSERT_TRUE(raw::planeOneSpan(xr, 2, 2, 8, 4, out, bx, by, bw, bh));
    EXPECT_EQ(bx, 2u);
    EXPECT_EQ(bw, 8u);
    EXPECT_EQ(by, 2u);
    EXPECT_EQ(bh, 4u);

    raw::Texture one;
    one.ycbcr = 1;
    one.onePlane = true;
    one.import = &import;
    EXPECT_FALSE(raw::planeOneSpan(one, 0, 0, 2, 2, out, bx, by, bw, bh));
}

}  // namespace
