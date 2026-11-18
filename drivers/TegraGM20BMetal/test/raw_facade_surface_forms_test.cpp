// SPDX-License-Identifier: MIT
// Textures over surfaces the raw path once answered nil for, as Metal makes them: each expectation is an M4 Max's
// (m2v-textures9 probes/xr.txt, ycbcr.txt).
#include "raw_facade_pass_scene.h"
#import "../raw/runtime/RawMetalXrPlanes.h"

namespace {

using namespace pass_scene;

struct PlaneShape {
    unsigned bytes, rows;
};

// A 'b3a8' surface of 64x64 whose planes lie one after another, each `rows` bytes a row
IOSurfaceRef planesSurface(std::initializer_list<PlaneShape> shapes)
{
    NSMutableArray *planes = [NSMutableArray array];
    unsigned offset = 0;
    for (const PlaneShape &p : shapes) {
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @64,
            (id)kIOSurfacePlaneHeight : @64,
            (id)kIOSurfacePlaneBytesPerElement : @(p.bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(p.rows),
            (id)kIOSurfacePlaneOffset : @(offset),
            (id)kIOSurfacePlaneSize : @(64 * p.rows)
        }];
        offset += 64 * p.rows;
    }
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(offset),
        (id)kIOSurfacePlaneInfo : planes
    });
}

MTLTextureDescriptor *sampled(uint32_t format, NSUInteger side)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:side
                                                                                height:side
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    return d;
}

const uint8_t *planeBytes(IOSurfaceRef s, size_t plane)
{
    return static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
}

// RGB10A8_2P_XR10 over a plane followed by another reads that plane's words and the next one's alpha bytes, over the last
// plane its words alone with alpha 0 (an M4 Max over planes 0, 1 and 2 of three: probes/xr.txt 1g, 1n, 1o)
void readsItsPlaneAndTheNext()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                                           {raw::xrConversionOf(554), MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = planesSurface({{4, 256}, {4, 256}, {4, 256}});
    ASSERT_TRUE(s);
    for (size_t plane = 0; plane < 3; plane++) {
        RMTexture *t = (RMTexture *)[device newTextureWithDescriptor:sampled(550, 64) iosurface:s plane:plane];
        ASSERT_TRUE(t != nil && t->_rawOk) << "plane " << plane;
        const raw::Texture &x = t->_raw;
        if (plane == 2) {
            EXPECT_FALSE(x.xrPlanes) << "the last plane: its words alone";
            EXPECT_EQ(x.cpu, planeBytes(s, 2));
            continue;
        }
        ASSERT_TRUE(x.xrPlanes && x.luma && x.chroma) << "plane " << plane;
        EXPECT_EQ(x.luma->cpu, planeBytes(s, plane)) << "its words";
        EXPECT_EQ(x.chroma->cpu, planeBytes(s, plane + 1)) << "the next plane's alpha bytes";
        EXPECT_FALSE(x.chroma->memoryless) << plane;
    }
    CFRelease(s);
}

// YCBCR8_420_2P reads luma from its plane and chroma from the next one where there is one, chroma codes 0 where there is
// none: over planes 1 and 2 of three, and over a surface of one plane (an M4 Max: probes/ycbcr.txt 3aj, 3s). A texture of no
// width is Metal's descriptor validation's stop (probes/ycbcr.txt, empty).
void ycbcrReadsItsPlaneAndTheNext()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef three = planesSurface({{1, 64}, {1, 64}, {2, 64}});
    IOSurfaceRef one = planesSurface({{4, 256}});
    ASSERT_TRUE(three && one);
    RMTexture *t = (RMTexture *)[device newTextureWithDescriptor:sampled(500, 32) iosurface:three plane:1];
    ASSERT_TRUE(t != nil && t->_rawOk) << "over plane 1 of three";
    ASSERT_TRUE(t->_raw.luma && t->_raw.chroma);
    EXPECT_EQ(t->_raw.luma->cpu, planeBytes(three, 1));
    EXPECT_EQ(t->_raw.chroma->cpu, planeBytes(three, 2));
    EXPECT_EQ(t->_raw.shape[14], 1u);
    t = (RMTexture *)[device newTextureWithDescriptor:sampled(500, 64) iosurface:one plane:0];
    ASSERT_TRUE(t != nil && t->_rawOk) << "over a surface of one plane";
    ASSERT_TRUE(t->_raw.luma);
    EXPECT_EQ(t->_raw.luma->cpu, planeBytes(one, 0));
    EXPECT_EQ(t->_raw.chroma, nullptr);
    EXPECT_EQ(t->_raw.shape[14], 0u) << "chroma codes 0";
    MTLTextureDescriptor *empty = sampled(500, 64);
    empty.width = 0;
    EXPECT_EQ([device newTextureWithDescriptor:empty iosurface:one plane:0], nil);
    CFRelease(three);
    CFRelease(one);
}

}  // namespace

TEST(RawFacadeSurfaceForms, RGB10A8XrReadsItsPlaneAndTheNext)
{
    switch_ios::test::ExpectPassesInFreshProcess(readsItsPlaneAndTheNext);
}

TEST(RawFacadeSurfaceForms, YcbcrReadsItsPlaneAndTheNext)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrReadsItsPlaneAndTheNext);
}
