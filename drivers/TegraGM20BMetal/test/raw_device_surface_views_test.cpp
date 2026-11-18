// SPDX-License-Identifier: MIT
// Textures over a buffer's or a surface's memory and their views (createBufferTexture, createTexelBuffer,
// createSurfaceTexture, createSurfaceTexelBuffer, createYcbcrTexture): the header each samples and the memory it names.
#include "raw_device_views_harness.h"
#include "../raw/runtime/raw_xr.h"
#include "../raw/runtime/raw_ycbcr.h"
#include <cstdlib>
#include <unistd.h>

namespace {

using namespace views;

// A view of an extended-range texture in its own format samples a view of the texture's stand-in (the same memory, a
// header of its own for its swizzle); one as its sRGB twin reads the same codes through a stand-in of its own, laid out
// as the texture is (every level), freed with the view
TEST_F(DeviceViews, AnExtendedRangeViewSamplesTheStandInOfItsTransfer)
{
    addOwnState("raw.xr.xr_bgr10_frag", 115);
    addOwnState("raw.xr.xr_bgr10_srgb_frag", 115);
    raw::Texture t, same, other;
    ASSERT_TRUE(dev.createMipTexture(32, 16, 554, 3, t));
    ASSERT_TRUE(dev.createStandIn(t));
    ASSERT_TRUE(dev.createView(t, 554, same));
    ASSERT_NE(same.standIn, nullptr);
    EXPECT_EQ(same.standIn->va, t.standIn->va) << "the texture's stand-in, no second one";
    EXPECT_NE(same.standIn->tic, t.standIn->tic);
    EXPECT_EQ(same.standIn->standIn, nullptr);
    ASSERT_TRUE(dev.createView(t, 555, other));
    EXPECT_EQ(other.va, t.va) << "the texture's codes";
    ASSERT_NE(other.standIn, nullptr);
    raw::Texture *own = other.standIn->standIn;
    ASSERT_NE(own, nullptr) << "BGR10_XR_sRGB reads other values from the same codes";
    EXPECT_EQ(other.standIn->va, own->va);
    EXPECT_NE(own->va, t.standIn->va);
    ASSERT_NE(own->chain, nullptr);
    EXPECT_EQ(own->chain->layout.levels, 3u) << "of the texture's whole layout";
    const uint32_t ownTic = own->tic;
    dev.destroyTexture(other);
    EXPECT_EQ(raw::DeviceHarness::header(dev, ownTic), raw::DeviceHarness::header(dev, 0)) << "freed with the view";
    EXPECT_NE(header(*t.standIn), raw::DeviceHarness::header(dev, 0)) << "the texture's own kept";
    dev.destroyTexture(same);
    dev.destroyTexture(t);
}

// A view of a texture over a buffer whose rows lie 16 but not 32 bytes apart samples a view of the texture's pool image,
// its own header there; its texels stay the buffer's
TEST_F(DeviceViews, AViewOfATextureSampledThroughAPoolImageSamplesAViewOfIt)
{
    raw::Buffer b;
    ASSERT_TRUE(dev.createBuffer(4096, b));
    raw::Texture t, v;
    ASSERT_TRUE(dev.createBufferTexture(b, 48, 12, 8, 48, kRGBA8, t));
    ASSERT_NE(t.sampled, nullptr);
    ASSERT_TRUE(dev.createView(t, 73 /* RGBA8Uint */, v));
    ASSERT_NE(v.sampled, nullptr);
    EXPECT_EQ(v.va, t.va);
    EXPECT_EQ(v.sampled->va, t.sampled->va);
    EXPECT_EQ(v.tic, v.sampled->tic);
    EXPECT_NE(v.tic, t.tic);
    const uint32_t tic = v.tic;
    dev.destroyTexture(v);
    EXPECT_EQ(raw::DeviceHarness::header(dev, tic), raw::DeviceHarness::header(dev, 0));
    EXPECT_NE(header(t), raw::DeviceHarness::header(dev, 0));
    dev.destroyTexture(t);
    dev.destroyBuffer(b);
}

// A linear 2D array over a buffer (Core Animation's newLinearTextureWithDescriptor:): one pitch header of TWO_D_ARRAY over
// its layers, which lie pitch x height apart (the pitch header has no array stride); rows off 32 bytes make none
TEST_F(DeviceViews, ALinearArrayIsOnePitchHeaderOfItsLayers)
{
    raw::Buffer b;
    ASSERT_TRUE(dev.createBuffer(1 << 16, b));
    raw::Texture t, off;
    ASSERT_TRUE(dev.createBufferTexture(b, 256, 16, 8, 64, kRGBA8, t, 3));
    EXPECT_EQ(t.layers, 3u);
    EXPECT_EQ(t.layerBytes, 64u * 8);
    const auto w = header(t);
    EXPECT_EQ(field(w, 87, 85), 2u) << "HEADER_VERSION PITCH";
    EXPECT_EQ(textureType(w), 5u) << "TWO_D_ARRAY";
    EXPECT_EQ(field(w, 189, 176), 2u) << "DEPTH_MINUS_ONE";
    EXPECT_EQ(field(w, 111, 96), 64u >> 5) << "PITCH_BITS20TO5";
    EXPECT_FALSE(dev.createBufferTexture(b, 256, 12, 8, 48, kRGBA8, off, 3)) << "rows off 32 bytes";
    EXPECT_FALSE(dev.createBufferTexture(b, 0, 16, 8, 64, kRGBA8, off, 200)) << "past the buffer";
    dev.destroyTexture(t);
    dev.destroyBuffer(b);
}

// A block-compressed texture over a surface's rows of blocks (ASTC 4x4: 16 bytes a block) is sampled in place under a pitch
// header of its texels, its rows the surface's rows of blocks; rows 16 but not 32 bytes apart through its block-linear pool
// image
TEST_F(DeviceViews, ACompressedSurfacesBlocksAreSampledInPlace)
{
    const size_t page = size_t(getpagesize());
    void *base = aligned_alloc(page, 4 * page);
    raw::Texture t, odd;
    ASSERT_TRUE(dev.createSurfaceTexture(0, base, 4 * page, nullptr, 0, 30, 20, 128, kASTC4x4, t, false));
    EXPECT_EQ(t.width, 8u) << "blocks a row";
    EXPECT_EQ(t.height, 5u) << "rows of blocks";
    EXPECT_FALSE(t.blockLinear);
    EXPECT_EQ(t.sampled, nullptr);
    const auto w = header(t);
    EXPECT_EQ(field(w, 87, 85), 2u) << "HEADER_VERSION PITCH";
    EXPECT_EQ(field(w, 143, 128), 29u) << "WIDTH_MINUS_ONE in texels";
    EXPECT_EQ(field(w, 175, 160), 19u) << "HEIGHT_MINUS_ONE in texels";
    EXPECT_EQ(field(w, 111, 96), 128u >> 5) << "PITCH_BITS20TO5: a row of blocks";
    ASSERT_TRUE(dev.createSurfaceTexture(0, base, 4 * page, nullptr, 0, 30, 20, 144, kASTC4x4, odd, false));
    ASSERT_NE(odd.sampled, nullptr);
    EXPECT_TRUE(odd.sampled->blockLinear);
    dev.destroyTexture(odd);
    dev.destroyTexture(t);
    free(base);
}

// A slice of a linear 2D array over a buffer is its own pitch header over that layer's rows (pitch x height on), as 2D or
// a one-layer array; two slices are a TWO_D_ARRAY of their own
TEST_F(DeviceViews, ASliceOfALinearArrayIsAPitchHeaderOfItsLayer)
{
    raw::Buffer b;
    ASSERT_TRUE(dev.createBuffer(1 << 16, b));
    raw::Texture t, one, two;
    ASSERT_TRUE(dev.createBufferTexture(b, 256, 16, 8, 64, kRGBA8, t, 3));
    ASSERT_TRUE(dev.createPartView(t, kRGBA8, raw::fx::kTexTwoD, 0, 1, 1, 1, one));
    EXPECT_EQ(one.va, t.va + t.layerBytes);
    const auto w = header(one);
    EXPECT_EQ(uint64_t(field(w, 63, 37)) << 5 | uint64_t(field(w, 79, 64)) << 32, t.va + t.layerBytes)
        << "ADDRESS_BITS31TO5: the layer's first row";
    EXPECT_EQ(field(header(one), 87, 85), 2u) << "HEADER_VERSION PITCH";
    ASSERT_TRUE(dev.createPartView(t, kRGBA8, raw::fx::kTexTwoDArray, 0, 1, 1, 2, two));
    EXPECT_EQ(textureType(header(two)), 5u) << "TWO_D_ARRAY";
    EXPECT_EQ(depthMinusOne(header(two)), 1u);
    raw::Texture past;
    EXPECT_FALSE(dev.createPartView(t, kRGBA8, raw::fx::kTexTwoDArray, 0, 1, 2, 2, past)) << "past its last layer";
    for (raw::Texture *x : {&two, &one, &t})
        dev.destroyTexture(*x);
    dev.destroyBuffer(b);
}

// An extended-range texel buffer (BGR10_XR) is raw's: its codes under a texel buffer header, and the stand-in a draw samples,
// its RGBA16Float texels in order under a texel buffer header of their own, drawn from the codes copied into an image of
// the same rows (raw_xr.h); RGB10A8_2P_XR10's reads its words, its alpha bytes 16 KiB past them as an M4 Max lays them
TEST_F(DeviceViews, AnExtendedRangeTexelBufferHasAStandInOfItsTexelsInOrder)
{
    addOwnState("raw.xr.xr_bgr10_frag", 115);
    addOwnState("raw.xr.xr_two_planes_frag", 115);
    raw::Buffer b;
    ASSERT_TRUE(dev.createBuffer(1 << 16, b));
    raw::Texture words, planes;
    ASSERT_TRUE(dev.createTexelBuffer(b, 16, 300, 554, words));
    EXPECT_EQ(words.type, raw::fx::kTexOneDBuffer);
    ASSERT_NE(words.standIn, nullptr);
    const raw::Texture &s = *words.standIn;
    EXPECT_EQ(s.format, raw::kXrStandInFormat);
    EXPECT_EQ(s.pitch, s.width * s.bpp) << "its rows its texels back to back";
    EXPECT_GE(uint64_t(s.width) * s.height, 300u);
    EXPECT_EQ(textureType(header(s)), raw::fx::kTexOneDBuffer);
    EXPECT_EQ(field(header(s), 143, 128), 299u) << "WIDTH_MINUS_ONE: the buffer's texels";
    EXPECT_EQ(field(header(s), 63, 32), uint32_t(s.va));
    ASSERT_NE(s.luma, nullptr);
    EXPECT_EQ(s.luma->format, 554u);
    EXPECT_EQ(s.luma->width, s.width);
    EXPECT_EQ(s.luma->pitch, s.width * 4u);
    ASSERT_TRUE(dev.createTexelBuffer(b, 0, 100, 550, planes));
    EXPECT_EQ(planes.bpp, 4u) << "its words";
    EXPECT_EQ(planes.storageRange().bytes, raw::texelBufferAlphaOffset(100) + 100) << "its alpha bytes";
    EXPECT_EQ(header(planes)[0], raw::formatInfo(53)->tic0) << "R32Uint: a word a texel";
    ASSERT_NE(planes.standIn, nullptr);
    ASSERT_TRUE(planes.standIn->luma && planes.standIn->chroma);
    EXPECT_EQ(planes.standIn->chroma->pitch, planes.standIn->width) << "an alpha byte a texel";
    raw::Texture short_;
    EXPECT_FALSE(dev.createTexelBuffer(b, 0, 16384, 550, short_)) << "its alpha bytes past the buffer";
    dev.destroyTexture(planes);
    dev.destroyTexture(words);
    dev.destroyBuffer(b);
}

// A texture buffer over a surface plane reads its bytes in order from the plane's first: a texel buffer header there
TEST_F(DeviceViews, ATexelBufferOverASurfaceIsAHeaderOfItsBytesInOrder)
{
    const size_t page = size_t(getpagesize());
    void *base = aligned_alloc(page, 4 * page);
    raw::Texture t, off;
    ASSERT_TRUE(dev.createSurfaceTexelBuffer(0, base, 4 * page, nullptr, 256, 1000, kRGBA8, t));
    EXPECT_EQ(textureType(header(t)), raw::fx::kTexOneDBuffer);
    EXPECT_EQ(field(header(t), 143, 128), 999u);
    EXPECT_EQ(t.va & (page - 1), 256u);
    EXPECT_FALSE(dev.createSurfaceTexelBuffer(0, base, 4 * page, nullptr, 8, 16, kRGBA8, off)) << "off 16";
    EXPECT_FALSE(dev.createSurfaceTexelBuffer(0, base, 4 * page, nullptr, 0, uint32_t(page) + 1, kRGBA8, off))
        << "past the surface";
    dev.destroyTexture(t);
    free(base);
}

// YCBCR10_444_1P (504) over a surface of words: luma and chroma two headers of the one plane as RGB10A2Unorm, chroma
// reading its green and blue (Cb, Cr); YCBCR8_422_1P_REV (562) splits its pairs Cb Y0 Cr Y1 (ycbcroracle: both exact)
TEST_F(DeviceViews, OnePlaneYcbcrFormatsReadTheirOwnLayout)
{
    addOwnState("raw.ycbcr.ycbcr_converted_frag", 115);
    const size_t page = size_t(getpagesize());
    void *base = aligned_alloc(page, 16 * page);
    raw::YcbcrFacts f;
    f.pixelFormat = 504;
    f.fourcc = 'v410';
    f.width = f.surfaceWidth = 64;
    f.height = f.surfaceHeight = 32;
    f.pitch[0] = 256;
    f.allocBytes = f.mappedBytes = 16 * page;
    f.simple = true;
    raw::YcbcrPlan plan;
    ASSERT_EQ(raw::planYcbcr(f, plan), nullptr);
    EXPECT_TRUE(plan.chromaGreenBlue && !plan.onePlane);
    raw::Texture words;
    ASSERT_TRUE(dev.createYcbcrTexture(0, base, 16 * page, nullptr, plan, 64, 32, words));
    ASSERT_TRUE(words.luma && words.chroma);
    // (a surface of ID 0 is imported anew each time: compare places in the import, not addresses)
    EXPECT_EQ(words.luma->va - words.luma->import->bo.va, words.chroma->va - words.chroma->import->bo.va) << "one plane";
    const uint32_t lumaWord0 = header(*words.luma)[0], chromaWord0 = header(*words.chroma)[0];
    EXPECT_EQ(chromaWord0 >> 19 & 7, lumaWord0 >> 22 & 7) << "X_SOURCE: what luma's Y_SOURCE reads, green";
    EXPECT_EQ(chromaWord0 >> 22 & 7, lumaWord0 >> 25 & 7) << "Y_SOURCE: what luma's Z_SOURCE reads, blue";
    f.pixelFormat = 562;
    f.fourcc = '2vuy';
    f.pitch[0] = 128;
    raw::YcbcrPlan pairs;
    ASSERT_EQ(raw::planYcbcr(f, pairs), nullptr);
    EXPECT_TRUE(pairs.onePlane && pairs.pairsReversed);
    raw::Texture reversed;
    ASSERT_TRUE(dev.createYcbcrTexture(0, base, 16 * page, nullptr, pairs, 64, 32, reversed));
    EXPECT_TRUE(reversed.onePlane && reversed.pairsReversed);
    dev.destroyTexture(reversed);
    dev.destroyTexture(words);
    free(base);
}

// A converted image is viewed as a one-layer 2D array: its image header copied, TWO_D_ARRAY written
TEST_F(DeviceViews, AConvertedImageIsAOneLayerArrayWhole)
{
    addOwnState("raw.ycbcr.ycbcr_converted_frag", 115);
    const size_t page = size_t(getpagesize());
    void *base = aligned_alloc(page, 16 * page);
    raw::YcbcrFacts f;
    f.pixelFormat = 500;
    f.fourcc = '420v';
    f.planes = 2;
    f.width = f.surfaceWidth = 64;
    f.height = f.surfaceHeight = 32;
    f.offset[1] = 64 * 32;
    f.pitch[0] = f.pitch[1] = 64;
    f.allocBytes = f.mappedBytes = 16 * page;
    f.simple = true;
    raw::YcbcrPlan plan;
    ASSERT_EQ(raw::planYcbcr(f, plan), nullptr);
    raw::Texture t, layer;
    ASSERT_TRUE(dev.createYcbcrTexture(0, base, 16 * page, nullptr, plan, 64, 32, t));
    ASSERT_TRUE(dev.createPartView(t, t.format, raw::fx::kTexTwoDArray, 0, 1, 0, 1, layer));
    auto w = header(layer), own = header(t);
    EXPECT_EQ(textureType(w), 5u) << "TWO_D_ARRAY";
    w[4] &= ~(15u << 23);
    own[4] &= ~(15u << 23);
    EXPECT_EQ(w, own) << "the image's header but its type";
    dev.destroyTexture(layer);
    dev.destroyTexture(t);
    free(base);
}

}  // namespace
