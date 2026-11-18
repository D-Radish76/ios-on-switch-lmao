// SPDX-License-Identifier: MIT
// What Metal's blits move of a texture held through planes (raw_blit_planes.h), as an M4 Max under macOS 27.2.1 moves it
// with validation off: plane 0's bytes, planeUnit of them a texel
#include "../raw/runtime/raw_blit_planes.h"

#include <gtest/gtest.h>
#include <vector>

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
}

// RGB10A8_2P_XR10's words over a surface the kernel laid out block-linear (an M4 Max makes 550 over a planeless 'w30r' a
// BGR10_XR target was cleared in, and Metal's validation stops a texture copy of 550 with any other format): plane 0 is
// its own texels in that layout, a region inside them
TEST(BlitPlanes, ABlockLinearWordPlaneIsItsTexelsInItsLayout)
{
    Planes xr;
    xr.t.xrPlanes = true;
    xr.t.luma = &xr.luma;
    xr.luma.width = 16;
    xr.luma.bpp = 4;
    xr.luma.blockLinear = true;
    xr.luma.blockHeight = 1;
    xr.luma.layerBytes = 4096;
    xr.luma.surfaceX = 2;
    xr.luma.surfaceY = 3;
    raw::Texture out;
    ASSERT_TRUE(raw::planeBytes(xr.t, 4, out));
    EXPECT_TRUE(out.blockLinear);
    EXPECT_EQ(out.va, 0x10000u);
    EXPECT_EQ(out.pitch, 64u);
    EXPECT_EQ(out.width, 16u);
    EXPECT_EQ(out.height, 8u);
    EXPECT_EQ(out.format, 53u);
    EXPECT_EQ(out.blockHeight, 1u);
    EXPECT_EQ(out.surfaceX, 2u);
    EXPECT_EQ(out.surfaceY, 3u);
    EXPECT_TRUE(raw::planeRegionReadable(out, 8, 0, 8, 8));
    EXPECT_FALSE(raw::planeRegionReadable(out, 9, 0, 8, 8)) << "past its row's texels: no bytes of the layout";
    ASSERT_TRUE(raw::planeBytes(xr.t, 1, out));
    EXPECT_EQ(out.width, 64u);
    EXPECT_EQ(out.surfaceX, 8u);
    EXPECT_FALSE(raw::planeBytes(xr.t, 16, out)) << "its first texel off a 16-byte unit";
}

// Plane 1 as a blit between two textures held through planes moves it: its bytes from its first at the rows of plane 0
// the caller names, to the end of its surface's last page; none for a one-plane format or a plane alone's alpha
TEST(BlitPlanes, PlaneOneIsItsBytesAtPlaneZerosRows)
{
    raw::Import import;
    import.id = 7;
    import.bo.va = 0x10000;
    import.bo.bytes = 0x4000;
    raw::Texture luma, chroma, t;
    luma.import = chroma.import = &import;
    luma.va = 0x10000;
    luma.pitch = 48;
    luma.height = 8;
    luma.format = 10;
    chroma.va = 0x10400;
    chroma.pitch = 32;
    chroma.width = 8;
    chroma.height = 4;
    chroma.bpp = 2;
    t.ycbcr = 1;
    t.luma = &luma;
    t.chroma = &chroma;
    raw::Texture out;
    ASSERT_TRUE(raw::planeOneRows(t, luma.pitch, out));
    EXPECT_EQ(out.va, 0x10400u);
    EXPECT_EQ(out.pitch, 48u) << "plane 0's rows, never its own";
    EXPECT_EQ(out.bpp, 1u);
    EXPECT_EQ(out.storage.va, 0x10400u);
    EXPECT_EQ(out.storage.bytes, 0x4000u - 0x400u) << "on through the plane's end to its surface's last page";

    chroma.format = 53;  // a packed plane: bytes too, never whole words
    ASSERT_TRUE(raw::planeOneRows(t, luma.pitch, out));
    EXPECT_EQ(out.bpp, 1u);

    raw::Texture xr, alpha = chroma;
    alpha.bpp = 1;
    xr.xrPlanes = true;
    xr.luma = &luma;
    xr.chroma = &alpha;
    ASSERT_TRUE(raw::planeOneRows(xr, 128, out));
    EXPECT_EQ(out.pitch, 128u);
    alpha.memoryless = true;
    EXPECT_FALSE(raw::planeOneRows(xr, 128, out)) << "a plane alone's alpha";

    raw::Texture one;
    one.ycbcr = 1;
    one.onePlane = true;
    one.import = &import;
    EXPECT_FALSE(raw::planeOneRows(one, 64, out));
}

// The runs of a plane-1 copy whose rows end past either side's last page: an M4 Max moves the rows inside both, reads 0
// past the source's page and lands nothing past the destination's (probes/f1.txt A7), a row an end cuts byte by byte
TEST(BlitPlanes, PlaneOneRunsReadZeroPastTheSourceAndDropPastTheDestination)
{
    struct Run {
        uint32_t row, rows, copied, zeroed;
        bool operator==(const Run &o) const
        {
            return row == o.row && rows == o.rows && copied == o.copied && zeroed == o.zeroed;
        }
    };
    const auto runs = [](uint64_t readable, uint64_t writable) {
        std::vector<Run> out;
        // 16 x 8 from (8, 0) at rows 128 apart into (6, 0) at rows 128 apart
        raw::planeOneRuns(readable, 128, 8, 0, writable, 128, 6, 0, 16, 8, [&](const raw::PlaneOneRun &r) {
            out.push_back({r.row, r.rows, r.copied, r.zeroed});
        });
        return out;
    };
    EXPECT_EQ(runs(4096, 4096), (std::vector<Run>{{0, 8, 16, 0}}));
    EXPECT_EQ(runs(256, 256), (std::vector<Run>{{0, 2, 16, 0}})) << "plane 1's 256 bytes end the page";
    EXPECT_EQ(runs(200, 4096), (std::vector<Run>{{0, 2, 16, 0}, {2, 6, 0, 16}}));
    EXPECT_EQ(runs(140, 4096), (std::vector<Run>{{0, 1, 16, 0}, {1, 1, 4, 12}, {2, 6, 0, 16}}));
    EXPECT_EQ(runs(0, 4096), (std::vector<Run>{{0, 8, 0, 16}})) << "a source of no plane 1";
    EXPECT_EQ(runs(4096, 400), (std::vector<Run>{{0, 3, 16, 0}, {3, 1, 10, 0}})) << "row 3 runs from 390 to 406";
    EXPECT_EQ(runs(4096, 395), (std::vector<Run>{{0, 3, 16, 0}, {3, 1, 5, 0}}));
}

}  // namespace
