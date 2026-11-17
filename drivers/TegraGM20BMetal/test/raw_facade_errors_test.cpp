// SPDX-License-Identifier: MIT
// What Metal's validation layer would stop completes on the raw facade as Apple's driver completes it, nothing on m2v
// (raw_client_errors.h, measured on an M-series Mac with API validation off: every one of these command buffers
// completed with no error): a copy or fill past its buffer, indirect arguments past theirs, a state of the other
// four-channel 8-bit order or another sample count than its pass, sample positions past the pass's samples, and a state
// whose colour Apple writes as reinterpreted bits (its draw skipped). The kernel refusing a submission or the channel
// fails the command buffer Internal naming the kernel's answer (RawMetalPredicates.h predFailure), as a visibility result
// mode Metal's validation stops fails it (raw/plan/m2v_routes.tsv call.vis). A command buffer whose later job the kernel
// refused after earlier ones ran counts as not run; a refused reset of indirect commands wrote none of their words.
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_batch.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <string>

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm;
constexpr float kTriangle[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
};

// The facade over the pack of the BGRA8 state, and that state
Opened openPacked()
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(packKey(kPacked)));
    if (!device)
        return {};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error];
    return {device, state};
}

id<MTLTexture> target(id<MTLDevice> device, MTLPixelFormat format, NSUInteger samples = 1)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    return [device newTextureWithDescriptor:d];
}

MTLRenderPassDescriptor *pass(id<MTLTexture> t)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = t.sampleCount > 1 ? MTLStoreActionDontCare : MTLStoreActionStore;
    return p;
}

void drawInto(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *p, id<MTLRenderPipelineState> state)
{
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

NSString *path(id<MTLCommandBuffer> cb)
{
    return ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
}

// `cb` committed failed with `code` for predicate `pred`, and m2v never opened or asked
void expectError(id<MTLDevice> device, id<MTLCommandBuffer> cb, const char *pred, MTLCommandBufferError code)
{
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(std::string(cb.error.domain.UTF8String ?: ""), std::string(MTLCommandBufferErrorDomain.UTF8String));
    EXPECT_EQ(cb.error.code, code) << text(cb.error);
    EXPECT_EQ(std::string(path(cb).UTF8String ?: ""), std::string("error:") + pred);
    NSDictionary *counts = counters(device);
    EXPECT_EQ([counts[@"m2v"] intValue], 0) << text(counts);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

// As expectError, the command buffer one its predicate failed (RawMetalPredicates.h predFailure)
void expectFailed(id<MTLDevice> device, id<MTLCommandBuffer> cb, const char *pred, MTLCommandBufferError code)
{
    expectError(device, cb, pred, code);
    NSDictionary *counts = counters(device);
    NSString *failed = [NSString stringWithFormat:@"failed.%s", pred];
    EXPECT_EQ([counts[failed] intValue], 1) << text(counts);
}

// `cb` committed completed on the raw path, no error, m2v never opened or asked
void expectCompleted(id<MTLDevice> device, id<MTLCommandBuffer> cb)
{
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    EXPECT_EQ(std::string(path(cb).UTF8String ?: ""), "raw");
    NSDictionary *counts = counters(device);
    EXPECT_EQ([counts[@"m2v"] intValue], 0) << text(counts);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

void bufferCopyPastItsBuffer()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLBuffer> a = [o.device newBufferWithLength:4096 options:0], b = [o.device newBufferWithLength:4096 options:0];
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:1 << 20];
    [blit endEncoding];
    expectCompleted(o.device, cb);
}

void bufferFillPastItsBuffer()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLBuffer> a = [o.device newBufferWithLength:4096 options:0];
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:a range:NSMakeRange(0, 1 << 22) value:7];
    [blit endEncoding];
    expectCompleted(o.device, cb);
}

void indirectArgumentsPastTheirBuffer()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLBuffer> arguments = [o.device newBufferWithLength:16 options:0];
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(target(o.device, kPacked))];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:arguments indirectBufferOffset:4];
    [e endEncoding];
    expectCompleted(o.device, cb);
}

// Apple's driver draws the right colour (the target orders the channels)
void stateOfTheOtherChannelOrder()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    drawInto(cb, pass(target(o.device, MTLPixelFormatRGBA8Unorm)), o.state);
    expectCompleted(o.device, cb);
}

// Apple's driver writes the other format's bits: the draw is skipped, the command buffer completed
void stateOfAnotherColourFormat()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    drawInto(cb, pass(target(o.device, MTLPixelFormatRGBA16Float)), o.state);
    expectCompleted(o.device, cb);
}

void stateSamplesNotThePasss()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    drawInto(cb, pass(target(o.device, kPacked, 4)), o.state);
    expectCompleted(o.device, cb);
}

void samplePositionsPastThePasssSamples()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    MTLRenderPassDescriptor *p = pass(target(o.device, kPacked, 2));
    const MTLSamplePosition positions[4] = {{0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}};
    [p setSamplePositions:positions count:4];
    id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    expectCompleted(o.device, cb);
}

// Metal's validation stops each (MTLRenderCommandEncoder.h setVisibilityResultMode:offset:): a mode set in a pass of no
// visibility result buffer, an offset past the buffer, a mode MTLVisibilityResultMode lacks
void visibilityModesMetalStops()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil);
    id<MTLBuffer> results = [o.device newBufferWithLength:64 options:0];

    const struct {
        bool buffer;
        NSUInteger mode, offset;
    } stopped[] = {
        {false, MTLVisibilityResultModeBoolean, 0}, {true, MTLVisibilityResultModeCounting, 64}, {true, 7, 0}};

    for (const auto &s : stopped) {
        MTLRenderPassDescriptor *p = pass(target(o.device, kPacked));
        p.visibilityResultBuffer = s.buffer ? results : nil;
        id<MTLCommandBuffer> cb = [[o.device newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        [e setVisibilityResultMode:MTLVisibilityResultMode(s.mode) offset:s.offset];
        [e endEncoding];
        expectError(o.device, cb, "call.vis", MTLCommandBufferErrorInternal);
    }
}

// One command buffer completes first, so the channel is made before the kernel refuses the next submission
void drawnOnce(id<MTLCommandQueue> queue, const Opened &o)
{
    id<MTLCommandBuffer> first = [queue commandBuffer];
    drawInto(first, pass(target(o.device, kPacked)), o.state);
    [first commit];
    [first waitUntilCompleted];
    ASSERT_EQ(first.status, MTLCommandBufferStatusCompleted) << text(first.error);
}

void kernelRefusesTheSubmission()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    kernel_model::model.refuse = 1;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, pass(target(o.device, kPacked)), o.state);
    expectFailed(o.device, cb, "enc.kernel", MTLCommandBufferErrorInternal);
    EXPECT_NE(text(cb.error).find("the kernel refused its job (0x"), std::string::npos) << text(cb.error);
}

void kernelRefusesTheChannel()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    for (uint32_t s = 0; s < kernel_model::kSlots; s++)
        if (!kernel_model::model.slots[s].uid)
            kernel_model::model.slots[s].foreign = true;
    kernel_model::model.foreignTakesFreedSlot = true;
    for (uint32_t s = 0; s < kernel_model::kSlots; s++)
        if (kernel_model::model.slots[s].uid && !kernel_model::model.slots[s].foreign)
            kernel_model::model.kill(s + 1, 0x1f);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, pass(target(o.device, kPacked)), o.state);
    expectFailed(o.device, cb, "enc.nochannel", MTLCommandBufferErrorInternal);
}

// The submission after the model holds this many jobs is refused, once; each job completes as the model takes it
size_t gRefuseAfterJobs = 0;

void completeAndRefuseAfter()
{
    kernel_model::Model &m = kernel_model::model;
    m.fence(m.jobs.back().channel - 1).completed = m.jobs.back().seqno;
    if (m.jobs.size() == gRefuseAfterJobs)
        m.refuse = 1;
}

// A pass of a batch's draws (raw_batch.h) into `first`, then a pass of one draw into `second`: a job after the first's
id<MTLCommandBuffer> twoJobs(id<MTLCommandQueue> queue, const Opened &o, id<MTLTexture> first, id<MTLTexture> second)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(first)];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    for (uint32_t i = 0; i < raw::BatchWork::kOperations; i++)
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    drawInto(cb, pass(second), o.state);
    return cb;
}

MTLRenderPassDescriptor *loading(id<MTLTexture> t)
{
    MTLRenderPassDescriptor *p = pass(t);
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    return p;
}

// The kernel refuses the second job once the first ran: the command buffer fails once and its range counts as not run,
// so neither new target is marked written and each, its clear given back, is written to its first contents ahead of its
// next use
void kernelRefusesTheSecondJob()
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    drawnOnce(queue, o);
    id<MTLTexture> first = target(o.device, kPacked), second = target(o.device, kPacked);
    id<MTLCommandBuffer> cb = twoJobs(queue, o, first, second);
    __block int completions = 0;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        completions++;
    }];
    kernel_model::Model &m = kernel_model::model;
    const size_t jobs = m.jobs.size();
    gRefuseAfterJobs = jobs + 1;
    m.jobAdded = completeAndRefuseAfter;
    expectFailed(o.device, cb, "enc.kernel", MTLCommandBufferErrorInternal);
    m.jobAdded = nullptr;
    EXPECT_EQ(m.jobs.size(), jobs + 1) << "not a command buffer whose first job ran";
    EXPECT_EQ(m.refuse, 0u) << "no second job was submitted to refuse";
    EXPECT_EQ(completions, 1);
    EXPECT_FALSE(((RMTexture *)first)->_gpuDirty.load());
    EXPECT_FALSE(((RMTexture *)second)->_gpuDirty.load());
    NSDictionary *before = ask(o.device, "switchRawFresh");
    ASSERT_NE(before[@"filled"], nil);
    id<MTLCommandBuffer> next = [queue commandBuffer];
    drawInto(next, loading(first), o.state);
    drawInto(next, loading(second), o.state);
    expectCompleted(o.device, next);
    NSDictionary *after = ask(o.device, "switchRawFresh");
    EXPECT_EQ([after[@"filled"] unsignedLongLongValue] - [before[@"filled"] unsignedLongLongValue], 2u);
}

}  // namespace

TEST(RawFacadeErrors, ABufferCopyPastItsBufferCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(bufferCopyPastItsBuffer);
}

TEST(RawFacadeErrors, ABufferFillPastItsBufferCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(bufferFillPastItsBuffer);
}

TEST(RawFacadeErrors, IndirectArgumentsPastTheirBufferComplete)
{
    switch_ios::test::ExpectPassesInFreshProcess(indirectArgumentsPastTheirBuffer);
}

TEST(RawFacadeErrors, AStateOfTheOtherChannelOrderCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateOfTheOtherChannelOrder);
}

TEST(RawFacadeErrors, AStateOfAnotherColourFormatCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateOfAnotherColourFormat);
}

TEST(RawFacadeErrors, AStateOfOtherSamplesCompletes)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateSamplesNotThePasss);
}

TEST(RawFacadeErrors, SamplePositionsPastThePasssSamplesComplete)
{
    switch_ios::test::ExpectPassesInFreshProcess(samplePositionsPastThePasssSamples);
}

TEST(RawFacadeErrors, VisibilityModesMetalStopsFail)
{
    switch_ios::test::ExpectPassesInFreshProcess(visibilityModesMetalStops);
}

TEST(RawFacadeErrors, TheKernelRefusingTheSubmissionFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelRefusesTheSubmission);
}

TEST(RawFacadeErrors, TheKernelRefusingTheChannelFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelRefusesTheChannel);
}

TEST(RawFacadeErrors, TheKernelRefusingTheSecondJobLeavesTheRangeNotRun)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelRefusesTheSecondJob);
}

