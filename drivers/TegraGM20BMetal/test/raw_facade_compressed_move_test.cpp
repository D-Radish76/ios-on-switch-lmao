// SPDX-License-Identifier: MIT
// A private target made plain for want of compressed memory moves onto it on the submit worker (raw_private_comp.h
// compressedFromPlain) while another thread records passes into it, binds it for sampling and copies out of it, through
// the raw facade over the kernel model. The worker alone writes the texture's memory fields; the recording reads none of
// them, which the thread-sanitized build (raw_facade_compressed_move_tsan_test) holds to: any such read with no order to
// the move is its failure.
#include "raw_scale_frames.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

using scale_frames::opened;

constexpr int kRounds = 16;

uint64_t counted(id<MTLDevice> device, const char *selector, NSString *key)
{
    NSDictionary *d = ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)device, sel_registerName(selector));
    return [d[key] unsignedLongLongValue];
}

NSString *facts(id<MTLDevice> device, id<MTLTexture> texture)
{
    return ((NSString * (*)(id, SEL, id)) objc_msgSend)((id)device, sel_registerName("switchRawProbeFacts:"), texture);
}

id<MTLTexture> sharedTexture(id<MTLDevice> device, const scale_frames::Shape &shape)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:shape.width
                                                                                 height:shape.height
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:td];
}

TEST(RawFacadeCompressedMove, ARecordingThreadUsesATargetWhileTheWorkerMovesIt)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    const scale_frames::Shape shape{4, 24, 1, 704, 392};
    const uint64_t made = counted(device, "switchRawCompression", @"made");
    const auto target = scale_frames::targets(device, shape);
    ASSERT_TRUE([facts(device, target[0]) containsString:@"comptags=0"]) << "none of its size made: asked for";
    for (int i = 0; i < 500 && counted(device, "switchRawCompression", @"made") == made; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_GT(counted(device, "switchRawCompression", @"made"), made) << "no compressed memory was made";
    const uint64_t moves = counted(device, "switchRawCounters", @"private_compressed_moves");
    const std::vector<id<MTLTexture>> beside = {sharedTexture(device, shape)};
    id<MTLTexture> copy = sharedTexture(device, shape);
    id<MTLTexture> moved = target[0];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLRenderPipelineState> state = opened().state;
    id<MTLCommandBuffer> last = nil;
    // each command buffer committed with no wait, so the next one records while the worker submits it and moves the
    // target: nothing orders this thread's reads of the texture after the move but what the facade itself does
    std::thread recorder([&] {
        for (int round = 0; round < kRounds; round++) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            scale_frames::pass(cb, state, target, shape, 0, uint32_t(round));
            scale_frames::pass(cb, state, beside, shape, 0, uint32_t(round), nil, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentTexture:moved atIndex:0];
            });
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:moved
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(shape.width, shape.height, 1)
                        toTexture:copy
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [cb commit];
            last = cb;
        }
    });
    recorder.join();
    [last waitUntilCompleted];
    EXPECT_EQ(last.status, MTLCommandBufferStatusCompleted) << last.error.description.UTF8String;
    EXPECT_TRUE([facts(device, moved) containsString:@"comptags=1"]) << facts(device, moved).UTF8String;
    EXPECT_EQ(counted(device, "switchRawCounters", @"private_compressed_moves"), moves + 1);
}

}  // namespace
