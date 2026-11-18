// SPDX-License-Identifier: MIT
// An RGB8_444_2P (542) surface texture made to be sampled alone, as Core Animation's widget snapshots are, holds no stand-in;
// a pass into it renders on the raw path all the same (an M4 Max renders into a texture of any usage as into a render
// target: raw/test/ycbcrnorenderoracle.mm, m2v-passes2 probe/nort.mm), through a stand-in of its command buffer's.
#include "raw_facade_pass_scene.h"

namespace {

using namespace pass_scene;

constexpr NSUInteger kSide = 64;

// '_RB4': a byte of luma and two of chroma a texel
IOSurfaceRef fourFourFour()
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {1u, 2u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @(kSide),
            (id)kIOSurfacePlaneHeight : @(kSide),
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(kSide * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 1 ? 0 : 16384),
            (id)kIOSurfacePlaneSize : @(kSide * kSide * bytes)
        }];
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('_RB4')),
        (id)kIOSurfaceAllocSize : @(16384 + kSide * kSide * 2),
        (id)kIOSurfacePlaneInfo : planes
    });
}

id<MTLDevice> planarDevice()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                          {{"raw.two_plane.two_plane_load", MTLPixelFormatRGBA16Float},
                           {"raw.two_plane.two_plane_store", MTLPixelFormatR8Unorm}}));
}

RMTexture *planarTexture(id<MTLDevice> device, IOSurfaceRef s, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(542)
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = usage;
    return (RMTexture *)[device newTextureWithDescriptor:d iosurface:s plane:0];
}

void aSampledPlanarTextureRenders(MTLLoadAction load)
{
    id<MTLDevice> device = planarDevice();
    ASSERT_TRUE(device != nil);
    IOSurfaceRef s = fourFourFour();
    ASSERT_TRUE(s);
    RMTexture *t = planarTexture(device, s, MTLTextureUsageShaderRead);
    CFRelease(s);
    ASSERT_TRUE(t != nil && t->_rawOk && t->_raw.planar);
    EXPECT_EQ(t->_raw.planarStandIn, nullptr) << "a texture made to be sampled holds no stand-in";
    EXPECT_EQ(t->_raw.luma, nullptr);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = (id<MTLTexture>)t;
    p.colorAttachments[0].loadAction = load;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    expectRaw(device, run(device, p));
    EXPECT_EQ(t->_raw.planarStandIn, nullptr) << "nor after its pass: the stand-in was the command buffer's";
}

// The texture made with render-target usage keeps the stand-in it holds, and two passes into a sampled one and a target
// one in one command buffer each render through their own
void aSampledAndATargetPlanarTextureRenderInOneCommandBuffer()
{
    id<MTLDevice> device = planarDevice();
    ASSERT_TRUE(device != nil);
    IOSurfaceRef a = fourFourFour(), b = fourFourFour();
    ASSERT_TRUE(a && b);
    RMTexture *sampled = planarTexture(device, a, MTLTextureUsageShaderRead),
              *target = planarTexture(device, b, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    CFRelease(a);
    CFRelease(b);
    ASSERT_TRUE(sampled != nil && target != nil && sampled->_rawOk && target->_rawOk);
    ASSERT_NE(target->_raw.planarStandIn, nullptr);
    raw::Texture *held = target->_raw.planarStandIn;
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    for (RMTexture *t : {sampled, target}) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = (id<MTLTexture>)t;
        p.colorAttachments[0].loadAction = MTLLoadActionLoad;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[1].texture = (id<MTLTexture>)(t == sampled ? target : sampled);
        p.colorAttachments[1].loadAction = MTLLoadActionClear;
        p.colorAttachments[1].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    }
    [cb commit];
    [cb waitUntilCompleted];
    expectRaw(device, cb);
    EXPECT_EQ(target->_raw.planarStandIn, held);
    EXPECT_EQ(sampled->_raw.planarStandIn, nullptr);
}

}  // namespace

TEST(RawFacadePlanarTargets, ASampledPlanarTextureClearedAndStoredRenders)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        aSampledPlanarTextureRenders(MTLLoadActionClear);
    });
}

TEST(RawFacadePlanarTargets, ASampledPlanarTextureLoadedAndStoredRenders)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        aSampledPlanarTextureRenders(MTLLoadActionLoad);
    });
}

TEST(RawFacadePlanarTargets, ASampledAndATargetPlanarTextureRenderInOneCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSampledAndATargetPlanarTextureRenderInOneCommandBuffer);
}
