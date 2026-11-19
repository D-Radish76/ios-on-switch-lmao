// SPDX-License-Identifier: MIT
// The colour targets a pass binds for the attachments the pass-begin sites sent to m2v (raw_attachment_target.h): a slice of
// a linear 2D array as one pitch layer (a shadow where its address is off 128), a texture buffer's row (none past 16384
// texels), and a plane of a 3D texture laid out several planes a block, which no target addresses (the pass renders an
// image of it) and no 2D header reads.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_attachment_target.h"

#include <gtest/gtest.h>

namespace {

constexpr uint32_t kRGBA8 = 70, kA1BGR5 = 41;

raw::Texture linearArray(uint64_t va, uint32_t pitch, uint32_t layers)
{
    raw::Texture t;
    t.va = va;
    t.width = t.texelWidth = 64;
    t.height = t.texelHeight = 32;
    t.format = kRGBA8;
    t.bpp = 4;
    t.pitch = pitch;
    t.layers = layers;
    t.layerBytes = uint64_t(pitch) * 32;
    t.type = raw::fx::kTexTwoDArray;
    return t;
}

TEST(DevicePassTargets, ASliceOfALinearArrayIsOnePitchLayer)
{
    const raw::Texture t = linearArray(0x100000, 256, 3);
    raw::Texture part;
    ASSERT_TRUE(raw::attachmentTarget(t, 0, 2, part));
    EXPECT_EQ(part.va, t.va + 2 * t.layerBytes);
    EXPECT_FALSE(part.blockLinear);
    EXPECT_EQ(part.layers, 1u);
    EXPECT_EQ(part.type, raw::fx::kTexTwoD);
    EXPECT_TRUE(part.renderable);
    uint32_t w[8];
    EXPECT_FALSE(raw::attachmentHeader(part, 0, false, w)) << "a block-linear header of a pitch slice";
    EXPECT_FALSE(raw::attachmentTarget(t, 0, 3, part));
    EXPECT_FALSE(raw::attachmentTarget(t, 1, 0, part));
    // rows 288 bytes apart: a pitch off 128, rendered through a block-linear shadow
    const raw::Texture odd = linearArray(0x100000, 288, 2);
    ASSERT_TRUE(raw::attachmentTarget(odd, 0, 1, part));
    EXPECT_FALSE(part.renderable);
    EXPECT_TRUE(part.shadowTarget);
}

TEST(DevicePassTargets, ATextureBufferIsItsRowUpTo16384Texels)
{
    raw::Texture t;
    t.va = 0x200000;
    t.format = kRGBA8;
    t.bpp = 4;
    t.width = t.texelWidth = 4096;
    t.height = t.texelHeight = 1;
    t.pitch = 4096 * 4;
    t.type = raw::fx::kTexOneDBuffer;
    raw::Texture row;
    ASSERT_TRUE(raw::texelBufferTarget(t, row));
    EXPECT_EQ(row.type, raw::fx::kTexTwoD);
    EXPECT_EQ(row.height, 1u);
    EXPECT_EQ(row.va, t.va);
    EXPECT_TRUE(row.renderable);
    t.va += 16;
    ASSERT_TRUE(raw::texelBufferTarget(t, row));
    EXPECT_TRUE(row.shadowTarget);
    t.width = 16385;
    EXPECT_FALSE(raw::texelBufferTarget(t, row));
}

// A YCbCr texture buffer's image wider than 16384 texels: rows of 16384 back to back, which its conversion draws as one
// pitch target of its rows, an M4 Max reading texel i at its plane's (i % 16384, i / 16384)
TEST(DevicePassTargets, AWideTextureBufferIsRowsOf16384Texels)
{
    raw::Texture t;
    t.va = 0x200000;
    t.format = kRGBA8;
    t.bpp = 4;
    t.width = t.texelWidth = 20000;
    t.height = t.texelHeight = 1;
    t.pitch = 16384 * 4;
    t.type = raw::fx::kTexOneDBuffer;
    raw::Texture rows;
    ASSERT_TRUE(raw::texelBufferRows(t, rows));
    EXPECT_EQ(rows.type, raw::fx::kTexTwoD);
    EXPECT_EQ(rows.width, 16384u);
    EXPECT_EQ(rows.height, 2u);
    EXPECT_EQ(rows.pitch, 16384u * 4);
    EXPECT_TRUE(rows.renderable);
    EXPECT_FALSE(raw::texelBufferTarget(t, rows)) << "no pass of the app's renders into one past 16384 texels";
}

class DevicePassTargetsVolume : public harness::DeviceTest {};

TEST_F(DevicePassTargetsVolume, APlaneInsideABlockOfSeveralIsRenderedThroughAnImage)
{
    raw::Texture deep, flat, standIn;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, deep));
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, true, flat));
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kA1BGR5, false, standIn));
    ASSERT_GT(deep.blockDepth, 0u);
    EXPECT_TRUE(raw::planeInBlocks(deep, 0, 7));
    EXPECT_TRUE(raw::planeInBlocks(standIn, 0, 1));
    EXPECT_FALSE(raw::planeInBlocks(deep, 0, 8));
    EXPECT_FALSE(raw::planeInBlocks(flat, 0, 3)) << "one plane a block: a part of its own";
    raw::Texture part;
    EXPECT_TRUE(raw::attachmentTarget(flat, 0, 3, part));
    EXPECT_FALSE(raw::attachmentTarget(deep, 0, 3, part));
}

}  // namespace
