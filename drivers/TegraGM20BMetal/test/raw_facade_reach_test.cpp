// SPDX-License-Identifier: MIT
// The reach (raw_reach.h) of a pass loading a rotation-4 plane on the raw facade, each scenario a process of its own:
//  - one whose reach form needs the pack's reach state, which the pack lacks, fails its command buffer on raw (the driver's
//    error, MTLCommandBufferErrorInternal);
//  - an executed indirect command buffer's draw is reached by the GPU running its commands again by reference under the
//    reach state's pixel program, scissored to the one 32x32 tile of the 64x32 frame the client's scissor touches.
#include "raw_facade_harness.h"
#include "raw_reach_calls.h"
#include "bazel/testing/fresh_process.h"
#include "../raw/runtime/raw_reach.h"

#import <IOSurface/IOSurface.h>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Scene {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
    id<MTLTexture> plane;
    IOSurfaceRef surface;
};

// A device over `pack`, the pack's state, and a plane of rotation 4 whose frame is 64x32
Scene scene(const std::string &pack)
{
    Scene s = {};
    const std::string metallib = writeMetallib();
    s.device = open(pack);
    if (!s.device)
        return s;
    NSError *error = nil;
    id<MTLLibrary> library = [s.device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    s.state = library ? [s.device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error] : nil;
    s.surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:32 mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    [d setValue:@YES forKey:@"framebufferOnly"];
    [d setValue:@4 forKey:@"rotation"];
    s.plane = s.surface ? [s.device newTextureWithDescriptor:d iosurface:s.surface plane:0] : nil;
    return s;
}

// A pass loading and storing the plane, `body` its draws after the state and its vertices
template <class Body> id<MTLCommandBuffer> loadedPass(const Scene &s, Body body)
{
    id<MTLCommandBuffer> cb = [[s.device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = s.plane;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:s.state];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    body(e);
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

void reachWithoutItsState()
{
    const Scene s = scene(writePack(packKey(kPacked)));
    ASSERT_TRUE(s.device != nil && s.state != nil && s.plane != nil);
    id<MTLCommandBuffer> cb = loadedPass(s, [](id<MTLRenderCommandEncoder> e) {
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    });
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal) << text(cb.error);
    EXPECT_NE(text(cb.error).find("no reach state"), std::string::npos) << text(cb.error);
    NSDictionary *drivers = counters(s.device);
    EXPECT_EQ([drivers[@"m2v"] intValue], 0) << text(drivers);
    CFRelease(s.surface);
}

void anExecuteIsReachedByItsCommands()
{
    const Scene s = scene(writePack(packKey(kPacked), {}, {}, true, "",
                                    {{raw::kReachState, MTLPixelFormat(raw::kReachFormat)},
                                     {raw::kReachReduceState, MTLPixelFormat(raw::kReachFormat)},
                                     {raw::turnMaskedState(4), MTLPixelFormat(raw::turnCopyFormat(4))}}));
    ASSERT_TRUE(s.device != nil && s.state != nil && s.plane != nil);
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeDraw;
    d.inheritPipelineState = YES;
    d.inheritBuffers = YES;
    id<MTLIndirectCommandBuffer> icb = [s.device newIndirectCommandBufferWithDescriptor:d maxCommandCount:1 options:0];
    ASSERT_TRUE(icb != nil);
    [[icb indirectRenderCommandAtIndex:0] drawPrimitives:MTLPrimitiveTypeTriangle
                                             vertexStart:0
                                             vertexCount:3
                                           instanceCount:1
                                            baseInstance:0];
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = loadedPass(s, [&](id<MTLRenderCommandEncoder> e) {
        [e setScissorRect:MTLScissorRect{40, 4, 20, 20}];
        [e executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
    });
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    NSDictionary *drivers = counters(s.device);
    EXPECT_EQ([drivers[@"m2v"] intValue], 0) << text(drivers);

    const MTLResourceID resource = icb.gpuResourceID;
    uint64_t commands = 0;
    memcpy(&commands, &resource, sizeof commands);
    size_t byReference = 0;
    std::vector<uint32_t> words;
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++) {
        for (const TegraGM20BAbiPush &push : kernel_model::model.jobs[j].pushes)
            byReference += push.va == commands;
        words.insert(words.end(), kernel_model::model.jobs[j].words.begin(), kernel_model::model.jobs[j].words.end());
    }
    EXPECT_EQ(byReference, 2u) << "the commands run by reference in the reach pass and in the pass";
    const std::vector<DrawCall> calls = drawCalls(words);
    ASSERT_FALSE(calls.empty());
    DrawCall reachState;
    drawCalls(gRM->raw->ownPipeline(raw::kReachState)->methods, &reachState);
    EXPECT_EQ(calls[0].pixelProgram, reachState.pixelProgram) << "the reach pass's call first, under the reach state";
    EXPECT_EQ(calls[0].scissorX, 2u << 16 | 1u) << "tile (1, 0) alone";
    EXPECT_EQ(calls[0].scissorY, 1u << 16);
    CFRelease(s.surface);
}

}  // namespace

TEST(RawFacadeReach, AReachThatFailsFailsItsCommandBufferOnRaw)
{
    switch_ios::test::ExpectPassesInFreshProcess(reachWithoutItsState);
}

TEST(RawFacadeReach, AnExecutedIndirectCommandBufferIsReachedByItsCommands)
{
    switch_ios::test::ExpectPassesInFreshProcess(anExecuteIsReachedByItsCommands);
}
