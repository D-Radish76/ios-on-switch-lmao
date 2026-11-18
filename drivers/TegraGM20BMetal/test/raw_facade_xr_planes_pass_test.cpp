// SPDX-License-Identifier: MIT
// Passes into RGB10A8_2P_XR10 (550) colours that cannot all take their two planes (RawMetalEncoder.mm xrPlanesPassOf): no
// free colour index for an alpha plane, or a device-made 550 beside a surface's, whose pass texels an M4 Max keeps
// (HW Test gpu.raw.xr_planes_images, made case); and a multisampled BGRA10_XR resolved by the pack's state over its codes. Each runs
// on the raw path, nothing of it on m2v.
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_samples.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <string>

namespace {

using namespace harness;

constexpr uint32_t kSide = 64;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

std::string path(id<MTLCommandBuffer> cb)
{
    NSString *p = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
    return p.UTF8String ?: "";
}

id<MTLTexture> planesTexture(id<MTLDevice> device)
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kSide),
            (id)kIOSurfacePlaneHeight : @(kSide),
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(kSide * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : kSide * kSide * 4),
            (id)kIOSurfacePlaneSize : @(kSide * kSide * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(kSide * kSide * 5),
        (id)kIOSurfacePlaneInfo : planes
    });
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:kSide height:kSide mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
    if (surface)
        CFRelease(surface);
    return t;
}

id<MTLTexture> plain(id<MTLDevice> device, MTLPixelFormat format)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:kSide height:kSide mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:d];
}

void expectRaw(id<MTLDevice> device, MTLRenderPassDescriptor *p)
{
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    EXPECT_EQ(path(cb), "raw");
}

id<MTLDevice> openWithPlaneImages()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                          {{"raw.xr.xr_planes_merge_frag", MTLPixelFormatRG32Uint},
                           {"raw.xr.xr_planes_split_frag", MTLPixelFormatR32Uint}}));
}

// Seven BGRA8 colours and a surface's 550 leave its alpha plane no index: the 550 is drawn in an image of its planes
void a550WithNoFreeIndexIsDrawnInOneImage()
{
    id<MTLDevice> device = openWithPlaneImages();
    ASSERT_TRUE(device != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    for (NSUInteger c = 0; c < 7; c++) {
        p.colorAttachments[c].texture = plain(device, MTLPixelFormatBGRA8Unorm);
        p.colorAttachments[c].loadAction = MTLLoadActionClear;
        p.colorAttachments[c].storeAction = MTLStoreActionStore;
        ASSERT_TRUE(p.colorAttachments[c].texture != nil);
    }
    p.colorAttachments[7].texture = planesTexture(device);
    ASSERT_TRUE(p.colorAttachments[7].texture != nil);
    p.colorAttachments[7].loadAction = MTLLoadActionLoad;
    p.colorAttachments[7].storeAction = MTLStoreActionStore;
    expectRaw(device, p);
}

// A device-made 550 beside a surface's: a 550 of one image beside it, so the surface's is drawn in an image of its planes
void aDeviceMade550BesideASurfacesIsRaws()
{
    id<MTLDevice> device = openWithPlaneImages();
    ASSERT_TRUE(device != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = planesTexture(device);
    p.colorAttachments[1].texture = plain(device, MTLPixelFormat(550));
    ASSERT_TRUE(p.colorAttachments[0].texture != nil && p.colorAttachments[1].texture != nil);
    for (NSUInteger c = 0; c < 2; c++) {
        p.colorAttachments[c].loadAction = MTLLoadActionClear;
        p.colorAttachments[c].storeAction = MTLStoreActionStore;
    }
    expectRaw(device, p);
}

void aMultisampledBgra10XrResolvesByItsState()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{raw::resolveState(552, 4), MTLPixelFormatRGBA16Uint}}));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA10_XR
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.textureType = MTLTextureType2DMultisample;
    d.sampleCount = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> four = [device newTextureWithDescriptor:d];
    id<MTLTexture> into = plain(device, MTLPixelFormatBGRA10_XR);
    ASSERT_TRUE(four != nil && into != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = four;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    p.colorAttachments[0].resolveTexture = into;
    expectRaw(device, p);
}

}  // namespace

TEST(RawFacadeXrPlanesPass, AMultisampledBgra10XrResolvesByItsState)
{
    switch_ios::test::ExpectPassesInFreshProcess(aMultisampledBgra10XrResolvesByItsState);
}

TEST(RawFacadeXrPlanesPass, A550WithNoFreeIndexIsDrawnInOneImage)
{
    switch_ios::test::ExpectPassesInFreshProcess(a550WithNoFreeIndexIsDrawnInOneImage);
}

TEST(RawFacadeXrPlanesPass, ADeviceMade550BesideASurfacesIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aDeviceMade550BesideASurfacesIsRaws);
}
