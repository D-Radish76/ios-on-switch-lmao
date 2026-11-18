// SPDX-License-Identifier: MIT
// Textures over a buffer's or a surface's rows as Metal lays them out, made by the raw path or nil as Metal answers, with no
// m2v device: rows past their memory, a buffer texture at 16 bytes (a pool image), and a surface plane off 16 (Metal's GPU
// layout, its CPU rows its own).
#include "raw_facade_harness.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

void expectNoM2V(id<MTLDevice> device)
{
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

NSURL *fileURL(const std::string &path)
{
    return [NSURL fileURLWithPath:@(path.c_str())];
}

// A texture whose rows run past its buffer's or surface's memory, or a memoryless one over a buffer, is nil as Metal
// answers an invalid descriptor
void texturesPastTheirMemory()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:0 bytesPerRow:256], nil) << "16 KiB of rows in 4 KiB";
    d.height = 16;
    d.storageMode = MTLStorageModeMemoryless;
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:0 bytesPerRow:256], nil);
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(surface != nullptr);
    d.storageMode = MTLStorageModeShared;
    d.height = 4096;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:0], nil) << "rows past the allocation";
    d.height = 16;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:1], nil) << "a plane it does not have";
    CFRelease(surface);
    IOSurfaceRef narrow = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @60,
        (id)kIOSurfaceHeight : @16,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @248,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(narrow != nullptr);
    ASSERT_EQ(IOSurfaceGetBytesPerRow(narrow) & 15, 8u) << "rows 8 past a multiple of 16";
    d.width = 60;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:narrow plane:0], nil) << "Metal asserts on rows off 16";
    CFRelease(narrow);
    expectNoM2V(device);
}

// A texture over a buffer at an offset and rows 16 but not 32 bytes apart (boot 21's SwitchHWTest, 12x8 at offset 48) is
// sampled through a pool image the copy engine fills before the draw; one off 16, which Metal's validation stops, is nil
void bufferTexturesAt16()
{
    const raw::PackEntry sampled = {0, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:12
                                                                                height:8
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:d offset:48 bytesPerRow:48];
    ASSERT_TRUE(texture != nil);
    EXPECT_EQ(gOpens, 0) << "made by the raw path";
    uint8_t texel[4] = {1, 2, 3, 4};
    [texture replaceRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0 withBytes:texel bytesPerRow:4];
    EXPECT_EQ(memcmp(static_cast<uint8_t *>(buffer.contents) + 48 + 3 * 48 + 5 * 4, texel, 4), 0)
        << "its rows the buffer's";
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    EXPECT_EQ(drawSampling(device, state, texture, sampler).status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:56 bytesPerRow:48], nil) << "an offset off 16";
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:48 bytesPerRow:56], nil) << "rows off 16";
    expectNoM2V(device);
}

// A surface plane whose offset or rows are off 16 (any covering its row, as IOSurface makes them): Metal's GPU reads it from
// both rounded down to 16 and its CPU at the plane's own (m2v linear_plan.rs, measured), so the raw texture is made at the
// rounded layout and replaceRegion and getBytes take the plane's; rows that overlap once rounded are nil
void planesOff16()
{
    const raw::PackEntry sampled = {0, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    const auto plane = [](uint32_t width, uint32_t offset, uint32_t row) {
        return @{
            (id)kIOSurfacePlaneWidth : @(width),
            (id)kIOSurfacePlaneHeight : @8,
            (id)kIOSurfacePlaneBytesPerElement : @4,
            (id)kIOSurfacePlaneBytesPerRow : @(row),
            (id)kIOSurfacePlaneOffset : @(offset),
            (id)kIOSurfacePlaneSize : @(row * 8)
        };
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @12,
        (id)kIOSurfaceHeight : @8,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA')),
        (id)kIOSurfaceAllocSize : @4096,
        (id)kIOSurfacePlaneInfo : @[ plane(12, 0, 48), plane(12, 388, 52), plane(13, 1024, 60) ]
    });
    ASSERT_TRUE(surface != nullptr);
    ASSERT_EQ(IOSurfaceGetPlaneCount(surface), 3u);
    ASSERT_EQ(IOSurfaceGetBytesPerRowOfPlane(surface, 1), 52u);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:12
                                                                                height:8
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d iosurface:surface plane:1];
    ASSERT_TRUE(texture != nil);
    const uint8_t texel[4] = {1, 2, 3, 4};
    [texture replaceRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0 withBytes:texel bytesPerRow:4];
    const uint8_t *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(surface));
    EXPECT_EQ(memcmp(base + 388 + 3 * 52 + 5 * 4, texel, 4), 0) << "the CPU writes the plane's own rows";
    uint8_t back[4] = {};
    [texture getBytes:back bytesPerRow:4 fromRegion:MTLRegionMake2D(5, 3, 1, 1) mipmapLevel:0];
    EXPECT_EQ(memcmp(back, texel, 4), 0);
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    EXPECT_EQ(drawSampling(device, state, texture, sampler).status, MTLCommandBufferStatusCompleted);
    d.width = 13;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:2], nil) << "rows of 60 overlap once 48 apart";
    CFRelease(surface);
    expectNoM2V(device);
}

// A surface over the client's memory may start off a page: its texture is imported from that page, its GPU address as far
// into the page as the surface's base and its CPU rows the surface's (Device::createSurfaceTexture, Kernel::bindImport)
void surfaceOffAPage()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const size_t page = size_t(getpagesize());
    void *pages = nullptr;
    ASSERT_EQ(posix_memalign(&pages, page, 3 * page), 0);
    uint8_t *base = static_cast<uint8_t *>(pages) + 256;
    raw::Texture t;
    ASSERT_TRUE(gRM->raw->createSurfaceTexture(0, base, 64 * 64 * 4, nullptr, 0, 64, 64, 256,
                                               uint32_t(MTLPixelFormatBGRA8Unorm), t, false));
    ASSERT_TRUE(t.import != nullptr);
    EXPECT_EQ(t.cpu, base);
    EXPECT_EQ(t.va - t.import->bo.vaBase, 256u) << "the GPU reads from the surface's base, not its page's";
    gRM->raw->destroyTexture(t);
    free(pages);
    expectNoM2V(device);
}

// A texture over a surface whose import the kernel refuses is nil, with no m2v device asked for one: m2v's NVK imports the
// same pages through the same BoImport (boot C32's mediaanalysisd: a 360x360 BGRA surface it reads and writes)
void aRefusedImportIsNil()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @360,
        (id)kIOSurfaceHeight : @360,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @1472,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(surface != nullptr);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:360
                                                                                height:360
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    kernel_model::model.refuseImports = 1;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:0], nil);
    EXPECT_EQ(kernel_model::model.refuseImports, 0u) << "the kernel was asked";
    EXPECT_TRUE([device newTextureWithDescriptor:d iosurface:surface plane:0] != nil) << "the next import is taken";
    CFRelease(surface);
    expectNoM2V(device);
}

// BGR10_XR_sRGB and BGRA10_XR_sRGB are raw's: AegirPoster's 720x1280 shared target that it samples (boot C32 made it on m2v
// and replayed every pass into it there), and the same of BGRA10_XR_sRGB, each with the stand-in a draw samples
void anSrgbExtendedRangeTextureIsRaws()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_bgr10_srgb_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.xr.xr_bgra10_srgb_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    for (const MTLPixelFormat format : {MTLPixelFormatBGR10_XR_sRGB, MTLPixelFormatBGRA10_XR_sRGB}) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                     width:720
                                                                                    height:1280
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        RMTexture *t = (RMTexture *)[device newTextureWithDescriptor:d];
        ASSERT_TRUE(t != nil) << format;
        EXPECT_TRUE(t->_rawOk && t->_raw.standIn) << format;
    }
    expectNoM2V(device);
}

// A heap's size for an extended-range array and an RG8Unorm_sRGB texture, which the device makes beside its arena, is the
// raw path's footprint of the texture and the stand-in a draw samples, with no m2v device asked
void heapSizesOfStandInTextures()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = MTLPixelFormatBGR10_XR;
    d.width = 64;
    d.height = 32;
    d.arrayLength = 3;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead;
    EXPECT_GE([device heapTextureSizeAndAlignWithDescriptor:d].size, NSUInteger(64 * 32 * 3 * (4 + 8)))
        << "its codes and its half stand-in";
    d.textureType = MTLTextureType2D;
    d.arrayLength = 1;
    d.pixelFormat = MTLPixelFormatRG8Unorm_sRGB;
    EXPECT_GE([device heapTextureSizeAndAlignWithDescriptor:d].size, NSUInteger(64 * 32 * (2 + 4)));
    expectNoM2V(device);
}

// A view's CPU copies are its texture's, at the view's level and slice (a view of levels 1 and 2 of a Shared mipmapped
// texture, written and read at both), made on the raw path with no m2v twin asked
void aViewsCpuCopiesTakeItsTexturesPaths()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:64
                                                                                height:32
                                                                             mipmapped:YES];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    ASSERT_TRUE(texture != nil);
    RMTexture *view = (RMTexture *)[texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Uint
                                                              textureType:MTLTextureType2D
                                                                   levels:NSMakeRange(1, 2)
                                                                   slices:NSMakeRange(0, 1)];
    ASSERT_TRUE(view != nil && view->_rawOk);
    uint8_t texel[4] = {1, 2, 3, 4};
    for (NSUInteger level : {0u, 1u}) {
        [(id<MTLTexture>)view replaceRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:level withBytes:texel bytesPerRow:4];
        [(id<MTLTexture>)view getBytes:texel bytesPerRow:4 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:level];
    }
    expectNoM2V(device);
}

// An extended-range texture views as its sRGB twin on the raw path, and as a format of its texel size not at all, as Metal
// answers (m2v view_plan.rs, measured: plain Metal aborts the other pairs); a view of a part of an X32_Stencil8 texture,
// a device texture buffer and a heap's sizes for shapes no heap holds are raw's answers too
void viewsAndShapesAnsweredOnRaw()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.xr.xr_bgr10_frag", MTLPixelFormatRGBA16Float},
                                           {"raw.xr.xr_bgr10_srgb_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGR10_XR
                                                                                 width:64
                                                                                height:32
                                                                             mipmapped:YES];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    ASSERT_TRUE(texture != nil);
    RMTexture *srgb = (RMTexture *)[texture newTextureViewWithPixelFormat:MTLPixelFormatBGR10_XR_sRGB];
    ASSERT_TRUE(srgb != nil && srgb->_rawOk);
    EXPECT_TRUE(srgb->_raw.standIn != nullptr);
    RMTexture *level = (RMTexture *)[texture newTextureViewWithPixelFormat:MTLPixelFormatBGR10_XR_sRGB
                                                               textureType:MTLTextureType2D
                                                                    levels:NSMakeRange(1, 1)
                                                                    slices:NSMakeRange(0, 1)];
    ASSERT_TRUE(level != nil && level->_rawOk);
    EXPECT_EQ([texture newTextureViewWithPixelFormat:MTLPixelFormatR32Uint], nil) << "Metal aborts it";
    MTLTextureDescriptor *stencil = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatX32_Stencil8
                                                                                       width:32
                                                                                      height:32
                                                                                   mipmapped:NO];
    stencil.storageMode = MTLStorageModePrivate;
    stencil.usage = MTLTextureUsageShaderRead;
    RMTexture *x32 = (RMTexture *)[device newTextureWithDescriptor:stencil];
    ASSERT_TRUE(x32 != nil && x32->_rawOk);
    EXPECT_EQ(((id<MTLTexture>)x32).pixelFormat, MTLPixelFormatX32_Stencil8);
    MTLTextureDescriptor *line = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:100
                                                                             resourceOptions:0
                                                                                       usage:MTLTextureUsageShaderRead];
    RMTexture *buffer = (RMTexture *)[device newTextureWithDescriptor:line];
    ASSERT_TRUE(buffer != nil && buffer->_rawOk);
    EXPECT_EQ(buffer->_raw.type, raw::fx::kTexOneDBuffer);
    EXPECT_EQ([device heapTextureSizeAndAlignWithDescriptor:line].size, 400u);
    d.storageMode = MTLStorageModeMemoryless;
    EXPECT_EQ([device heapTextureSizeAndAlignWithDescriptor:d].size, 0u) << "no heap holds a memoryless texture";
    MTLTextureDescriptor *bfloat = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(116)
                                                                                    width:16
                                                                                   height:16
                                                                                mipmapped:NO];
    EXPECT_EQ([device newTextureWithDescriptor:bfloat], nil) << "no bfloat16 component in the GM20B";
    EXPECT_EQ([device heapTextureSizeAndAlignWithDescriptor:bfloat].size, 0u);
    expectNoM2V(device);
}

}  // namespace

TEST(RawFacadeTextureRows, ViewsAndShapesAreAnsweredOnRawWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(viewsAndShapesAnsweredOnRaw);
}

TEST(RawFacadeTextureRows, AViewsCpuCopiesTakeItsTexturesPathsWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(aViewsCpuCopiesTakeItsTexturesPaths);
}

TEST(RawFacadeTextureRows, AHeapSizesAStandInTextureWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(heapSizesOfStandInTextures);
}

TEST(RawFacadeTextureRows, ARefusedSurfaceImportIsNilWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRefusedImportIsNil);
}

TEST(RawFacadeTextureRows, AnSrgbExtendedRangeTextureIsRawsWithNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(anSrgbExtendedRangeTextureIsRaws);
}

TEST(RawFacadeTextureRows, ATexturePastItsMemoryOrOffItsRowsIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(texturesPastTheirMemory);
}

TEST(RawFacadeTextureRows, ABufferTextureAt16IsSampledThroughAPoolImage)
{
    switch_ios::test::ExpectPassesInFreshProcess(bufferTexturesAt16);
}

TEST(RawFacadeTextureRows, APlaneOff16IsMadeAtMetalsGpuLayoutWithItsCpuRows)
{
    switch_ios::test::ExpectPassesInFreshProcess(planesOff16);
}

TEST(RawFacadeTextureRows, ASurfaceOffAPageIsImportedFromItsPage)
{
    switch_ios::test::ExpectPassesInFreshProcess(surfaceOffAPage);
}
