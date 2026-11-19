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
    IOSurfaceRef three = planesSurface({{1, 64}, {1, 64}, {2, 128}});
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

// RGB10A8_2P_XR10 over planes whose offsets or rows are off 16: Metal's GPU reads, writes and blits them at both rounded down
// to 16, and its CPU copies take the word plane's own rows (an M4 Max: probes/xr.txt 5a-5n)
void planesOffSixteenAreRoundedForTheGpu()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_two_planes_frag", MTLPixelFormatRGBA16Float},
                                           {raw::xrConversionOf(554), MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef two = planesSurface({{4, 264}, {1, 72}});
    IOSurfaceRef one = planesSurface({{4, 264}});
    ASSERT_TRUE(two && one);
    const uint32_t word = 0x2abcdef1;
    for (IOSurfaceRef s : {two, one}) {
        RMTexture *t = (RMTexture *)[device newTextureWithDescriptor:sampled(550, 64) iosurface:s plane:0];
        ASSERT_TRUE(t != nil && t->_rawOk) << IOSurfaceGetPlaneCount(s) << " planes";
        const raw::Texture &words = t->_raw.xrPlanes ? *t->_raw.luma : t->_raw;
        EXPECT_EQ(words.pitch, 256u) << "rows of 264 as 256";
        if (s == two) {
            ASSERT_TRUE(t->_raw.xrPlanes);
            EXPECT_EQ(t->_raw.chroma->pitch, 64u) << "rows of 72 as 64";
        }
        EXPECT_EQ(t->_cpuRows, planeBytes(s, 0));
        EXPECT_EQ(t->_cpuPitch, 264u);
        [(id<MTLTexture>)t replaceRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0 withBytes:&word bytesPerRow:4];
        EXPECT_EQ(memcmp(planeBytes(s, 0) + 3 * 264 + 5 * 4, &word, 4), 0) << "the CPU writes the plane's own rows";
        uint32_t back = 0;
        [(id<MTLTexture>)t getBytes:&back bytesPerRow:4 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
        EXPECT_EQ(back, word);
    }
    CFRelease(two);
    CFRelease(one);
}

// An 8x8 '420v' of 8 MiB + 16 KiB, plane 0 bytes `rows` apart at `luma`, plane 1 pairs `rows` apart at `chroma`
IOSurfaceRef narrowPlanes(unsigned rows, unsigned luma, unsigned chroma)
{
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @8,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('420v')),
        (id)kIOSurfaceAllocSize : @((8u << 20) + 16384),
        (id)kIOSurfacePlaneInfo : @[
            @{
                (id)kIOSurfacePlaneWidth : @8,
                (id)kIOSurfacePlaneHeight : @8,
                (id)kIOSurfacePlaneBytesPerElement : @1,
                (id)kIOSurfacePlaneBytesPerRow : @(rows),
                (id)kIOSurfacePlaneOffset : @(luma),
                (id)kIOSurfacePlaneSize : @(8 * rows)
            },
            @{
                (id)kIOSurfacePlaneWidth : @4,
                (id)kIOSurfacePlaneHeight : @4,
                (id)kIOSurfacePlaneBytesPerElement : @2,
                (id)kIOSurfacePlaneBytesPerRow : @(rows),
                (id)kIOSurfacePlaneOffset : @(chroma),
                (id)kIOSurfacePlaneSize : @(4 * rows)
            }
        ]
    });
}

// A two-plane YCbCr texture's CPU copies move its luma codes alone, a code a texel at its plane's own offset and rows
// (8 at 8, which its GPU reads 1 MiB apart from 0), never its chroma; a packed 10-bit one's move nothing (an M4 Max:
// m2v-textures13 probes/c4misc/RESULT.txt P1, P2)
void ycbcrCpuCopiesMoveTheLumaPlaneAlone()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.ycbcr.ycbcr_packed_converted_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef narrow = narrowPlanes(8, 8, 4100), wide = narrowPlanes(16, 0, 4096);
    ASSERT_TRUE(narrow && wide);
    uint8_t *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(narrow));
    memset(base, 0xee, 8192);
    RMTexture *t = (RMTexture *)[device newTextureWithDescriptor:sampled(500, 8) iosurface:narrow plane:0];
    ASSERT_TRUE(t != nil && t->_rawOk);
    uint8_t codes[4 * 16];
    for (unsigned i = 0; i < sizeof codes; i++)
        codes[i] = uint8_t(i * 7 + 3);
    [(id<MTLTexture>)t replaceRegion:MTLRegionMake2D(2, 3, 4, 4) mipmapLevel:0 withBytes:codes bytesPerRow:16];
    for (unsigned a = 0; a < 8192; a++) {
        const unsigned y = (a - 8) / 8, x = (a - 8) % 8;
        const bool region = a >= 8 && y >= 3 && y < 7 && x >= 2 && x < 6;
        EXPECT_EQ(base[a], region ? codes[(y - 3) * 16 + x - 2] : 0xee) << "byte " << a;
    }
    uint8_t back[4 * 16];
    memset(back, 0x55, sizeof back);
    [(id<MTLTexture>)t getBytes:back bytesPerRow:16 fromRegion:MTLRegionMake2D(2, 3, 4, 4) mipmapLevel:0];
    for (unsigned i = 0; i < sizeof back; i++)
        EXPECT_EQ(back[i], i % 16 < 4 ? codes[i] : 0x55) << "buffer byte " << i;
    RMTexture *p = (RMTexture *)[device newTextureWithDescriptor:sampled(508, 8) iosurface:wide plane:0];
    ASSERT_TRUE(p != nil && p->_rawOk);
    uint8_t *bytes = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(wide));
    memset(bytes, 0xee, 8192);
    [(id<MTLTexture>)p replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:codes bytesPerRow:16];
    EXPECT_EQ(std::count(bytes, bytes + 8192, 0xee), 8192) << "a packed 10-bit texture's replaceRegion moves nothing";
    memset(back, 0x55, sizeof back);
    [(id<MTLTexture>)p getBytes:back bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    EXPECT_EQ(std::count(back, back + sizeof back, 0x55), long(sizeof back)) << "nor its getBytes";
    CFRelease(narrow);
    CFRelease(wide);
}

// A texture buffer of a YCbCr or packed 10-bit plane format over a surface's plane, which an M4 Max makes (validation on
// and off) and reads as a 2D texture of the format's row 0 (64 of 64 texels: m2v-textures13 probes/ytbuf/ytbuf.txt,
// ytread.txt): its converted image one linear row under a texel buffer's header
void ycbcrTexelBuffersAreTheirFirstRow()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.ycbcr.ycbcr_codes_frag", MTLPixelFormatRGBA8Unorm},
                                           {"raw.ycbcr.ycbcr_packed_plane_frag", MTLPixelFormatRGB10A2Unorm}}));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = narrowPlanes(64, 0, 4096);
    ASSERT_TRUE(s);
    for (const uint32_t format : {500u, 540u, 588u}) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                         width:8
                                                                                 resourceOptions:MTLResourceStorageModeShared
                                                                                         usage:MTLTextureUsageShaderRead];
        RMTexture *t = (RMTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
        ASSERT_TRUE(t != nil && t->_rawOk) << format << ": Metal makes it";
        const raw::Texture &x = t->_raw;
        EXPECT_TRUE(x.converted()) << format;
        EXPECT_EQ(x.type, raw::fx::kTexOneDBuffer) << format;
        EXPECT_FALSE(x.blockLinear) << format << ": a texel buffer's header reads linear memory";
        EXPECT_EQ(x.width, 8u) << format;
        EXPECT_EQ(x.height, 1u) << format;
        EXPECT_EQ(gRM->raw->headerWords(x.tic)[2] >> 21 & 7, 0u) << format << ": HEADER_VERSION SELECT_ONE_D_BUFFER";
    }
    CFRelease(s);
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

TEST(RawFacadeSurfaceForms, RGB10A8XrPlanesOffSixteenAreRoundedForTheGpu)
{
    switch_ios::test::ExpectPassesInFreshProcess(planesOffSixteenAreRoundedForTheGpu);
}

TEST(RawFacadeSurfaceForms, YcbcrCpuCopiesMoveTheLumaPlaneAlone)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrCpuCopiesMoveTheLumaPlaneAlone);
}

TEST(RawFacadeSurfaceForms, YcbcrTexelBuffersAreTheirFirstRow)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrTexelBuffersAreTheirFirstRow);
}
