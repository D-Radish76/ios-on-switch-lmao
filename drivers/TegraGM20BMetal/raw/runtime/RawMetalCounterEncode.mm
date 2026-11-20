// SPDX-License-Identifier: MIT
// Counter samples on the tape and in the raw encode: the stage boundaries a pass descriptor's sample buffer attachments name
// (the only sampling point supportsCounterSampling: answers YES for), and the blit encoder's resolve of samples on the GPU.
#import "RawMetalCounters.h"
#include <algorithm>
#include "raw_compute.h"
#include "raw_counter_kernel.h"

using CounterPoint = raw::counters::Point;

// The driver takes only its own buffer class (AGX::CounterSamplingContextGen1::setCounterSampleBuffer): another
// device's fails the command buffer InvalidResource, as every object of another device does
static RMCounterSampleBuffer *ownBuffer(RMCommandBuffer *cb, id<MTLCounterSampleBuffer> b, const char *what)
{
    if (!b || [(id)b isKindOfClass:[RMCounterSampleBuffer class]])
        return (RMCounterSampleBuffer *)b;
    rmCallInvalid(cb, PredCallCounter, 0, 0, what);
    return nil;
}

static void pend(RMCommandBuffer *cb, RMEncoder *e, RMCounterSampleBuffer *b, NSUInteger index, CounterPoint point)
{
    switch (raw::counters::sampleIndex(index, b->_sampleCount)) {
    case raw::counters::Index::None:
        return;
    case raw::counters::Index::Invalid:
        rmSkipped(PredCallCounter);
        SWITCH_REFUSE("raw", "counter.index", "Invalid sample index: %lu > %lu: not sampled", (unsigned long)index,
                      (unsigned long)b->_sampleCount);
        return;
    case raw::counters::Index::Sample:
        break;
    }
    [cb retain:b];
    rmPipelineVeto(cb, VetoCounters);
    e->_counterPending.push_back({0, point, uint32_t(index), b});
}

void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLRenderPassDescriptor *pass)
{
    if (!rmCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLRenderPassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (RMCounterSampleBuffer *b =
                ownBuffer(cb, a.sampleBuffer, "a render pass's counter sample buffer not made by this device")) {
            pend(cb, e, b, a.startOfVertexSampleIndex, CounterPoint::VertexStart);
            pend(cb, e, b, a.startOfFragmentSampleIndex, CounterPoint::FragmentStart);
            pend(cb, e, b, a.endOfVertexSampleIndex, CounterPoint::VertexEnd);
            pend(cb, e, b, a.endOfFragmentSampleIndex, CounterPoint::FragmentEnd);
        }
        a.sampleBuffer = nil;
    }
}

void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLComputePassDescriptor *pass)
{
    if (!rmCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLComputePassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (RMCounterSampleBuffer *b =
                ownBuffer(cb, a.sampleBuffer, "a compute pass's counter sample buffer not made by this device")) {
            pend(cb, e, b, a.startOfEncoderSampleIndex, CounterPoint::ComputeStart);
            pend(cb, e, b, a.endOfEncoderSampleIndex, CounterPoint::ComputeEnd);
        }
    }
}

void rmCounterMarks(RMCommandBuffer *cb, RMEncoder *e, MTLBlitPassDescriptor *pass)
{
    if (!rmCounterBuffersMade())
        return;
    for (NSUInteger i = 0; i < 4; i++) {
        MTLBlitPassSampleBufferAttachmentDescriptor *a = pass.sampleBufferAttachments[i];
        if (RMCounterSampleBuffer *b =
                ownBuffer(cb, a.sampleBuffer, "a blit pass's counter sample buffer not made by this device")) {
            pend(cb, e, b, a.startOfEncoderSampleIndex, CounterPoint::BlitStart);
            pend(cb, e, b, a.endOfEncoderSampleIndex, CounterPoint::BlitEnd);
        }
    }
}

void rmPlaceCounterMarks(RMCommandBuffer *cb, RMEncoder *e, size_t at, bool starts)
{
    auto &pending = e->_counterPending;
    for (RMCounterMark &m : pending)
        if (raw::counters::isStart(m.point) == starts) {
            m.at = uint32_t(at);
            cb->_counterMarks.push_back(m);
        }
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [&](const RMCounterMark &m) {
                                     return raw::counters::isStart(m.point) == starts;
                                 }),
                  pending.end());
}

static bool before(const RMCounterMark &m, size_t position)
{
    return m.at < position;
}

RMCounterEmit::RMCounterEmit(RMCommandBuffer *cb, size_t from, size_t to, raw::CommandBuffer &rc) : rc_(rc)
{
    const auto &marks = cb->_counterMarks;
    first_ = marks.data() + (std::lower_bound(marks.begin(), marks.end(), from, before) - marks.begin());
    last_ = marks.data() + (std::lower_bound(marks.begin(), marks.end(), to, before) - marks.begin());
}

const RMCounterMark *RMCounterEmit::begin(size_t position) const
{
    return std::lower_bound(first_, last_, position, before);
}

const RMCounterMark *RMCounterEmit::end(size_t position) const
{
    return std::lower_bound(first_, last_, position + 1, before);
}

bool RMCounterEmit::at(size_t position) const
{
    return begin(position) != end(position);
}

// The record's address, its buffer written by this submission
uint64_t RMCounterEmit::take(const RMCounterMark &m)
{
    RMBuffer *storage = m.buffer->_storage;
    useBuffer(&rc_, storage, true);
    storage->_gpuDirty.store(true, std::memory_order_release);
    return storage->_raw.va + uint64_t(m.index) * raw::counters::kRecordBytes;
}

void RMCounterEmit::started(size_t position, raw::RenderEncoder &e)
{
    for (const RMCounterMark *m = begin(position); m != end(position); m++)
        e.timestampAtStart(take(*m), m->point);
}

void RMCounterEmit::started(size_t position, raw::ComputeEncoder &e)
{
    for (const RMCounterMark *m = begin(position); m != end(position); m++)
        e.timestamp(take(*m));
}

void RMCounterEmit::started(size_t position, raw::BlitEncoder &e)
{
    for (const RMCounterMark *m = begin(position); m != end(position); m++)
        e.timestamp(take(*m), false);
}

void RMCounterEmit::ended(size_t position)
{
    raw::counters::Stamp stamps[8];
    size_t count = 0;
    for (const RMCounterMark *m = begin(position); m != end(position) && count < 8; m++)
        stamps[count++] = {take(*m), m->point};
    std::stable_sort(stamps, stamps + count, [](const raw::counters::Stamp &a, const raw::counters::Stamp &b) {
        return a.point < b.point;
    });
    rc_.timestamps(stamps, count);
}

static id<MTLComputePipelineState> resolveState(const char *name)
{
    NSError *error = nil;
    id<MTLDevice> device = gRM->device;
    id<MTLLibrary> library = [device newLibraryWithSource:@(raw::counters::kResolveSource) options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@(name)];
    id<MTLComputePipelineState> state = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!state)
        SWITCH_REFUSE_DETAIL("raw", "counter.resolve_state", uintptr_t(name),
                             "the counter resolve kernel %s was not made (%s): no blit resolve of it", name,
                             error.localizedDescription.UTF8String ?: "no function");
    return state;
}

id<MTLComputePipelineState> rmCounterResolveState()
{
    static id<MTLComputePipelineState> state;
    static std::once_flag once;
    std::call_once(once, [] {
        state = resolveState(raw::counters::kResolveFunction);
    });
    return state;
}

// The resolve into a destination offset off 4 bytes, made at the first such resolve
static id<MTLComputePipelineState> resolveBytesState()
{
    static id<MTLComputePipelineState> state;
    static std::once_flag once;
    std::call_once(once, [] {
        state = resolveState(raw::counters::kResolveBytesFunction);
    });
    return state;
}

@interface RMBlitEncoder (Counters)
@end
@implementation RMBlitEncoder (Counters)

// The driver's copy of the samples' words on the GPU, here a dispatch of the resolve kernel between the encoder's commands
// before and after it: the encoder is ended, the kernel's compute encoder recorded, and the encoder begun again
- (void)resolveCounters:(id<MTLCounterSampleBuffer>)sampleBuffer
                inRange:(NSRange)range
      destinationBuffer:(id<MTLBuffer>)destination
      destinationOffset:(NSUInteger)offset
{
    RMCounterSampleBuffer *b =
        ownBuffer(_cb, sampleBuffer, "resolveCounters: a counter sample buffer not made by this device");
    // an M4 Max resolves nothing of a nil one
    if (!sampleBuffer) {
        rmSkipped(PredCallCounter);
        SWITCH_REFUSE_DETAIL("raw", "counter.resolve", 1, "resolveCounters: a nil counter sample buffer: nothing");
    }
    if (!b)
        return;
    if (!raw::counters::withinSamples(range.location, range.length, b->_sampleCount)) {
        rmSkipped(PredCallCounter);
        SWITCH_REFUSE("raw", "counter.resolve",
                      "Cannot resolve MTLCounter data: out of bounds access (%lu of %lu samples)",
                      (unsigned long)NSMaxRange(range), (unsigned long)b->_sampleCount);
        return;
    }
    if (!range.length)
        return;
    RMBuffer *into = (RMBuffer *)destination;
    if (![(id)into isKindOfClass:[RMBuffer class]])
        return rmCallInvalid(_cb, PredCallBufForeign, offset, 0,
                             destination ? "resolveCounters: a destination buffer not made by this device"
                                         : "resolveCounters: a nil destination buffer");
    // an M4 Max writes the samples at an offset off 4 bytes byte for byte from there
    const uint32_t shift = uint32_t(offset % 4);
    id<MTLComputePipelineState> state = shift ? resolveBytesState() : rmCounterResolveState();
    if (shift) {
        rmSkipped(PredCallCounter);
        SWITCH_REFUSE("raw", "counter.resolve_into", "resolveCounters: into offset %lu, off 4 bytes: %s",
                      (unsigned long)offset, state ? "resolved there" : "not resolved");
    }
    // an M4 Max resolves the samples the destination holds past the offset
    const uint64_t fits = offset < into->_raw.length ? (into->_raw.length - offset) / raw::counters::kSampleBytes : 0;
    if (range.length > fits) {
        rmSkipped(PredCallCounter);
        SWITCH_REFUSE_DETAIL("raw", "counter.resolve_into", 1,
                             "resolveCounters: %lu samples into %llu bytes at %lu: the %llu it holds resolved",
                             (unsigned long)range.length, (unsigned long long)into->_raw.length, (unsigned long)offset,
                             (unsigned long long)fits);
        range.length = NSUInteger(fits);
        if (!range.length)
            return;
    }
    if (!state)
        return;
    const raw::counters::ResolveParams params = {uint32_t(range.length)};
    const raw::counters::ResolveBytesParams shifted = {uint32_t(range.length), shift};
    const RMClockAnchors &anchors = rmClockAnchors();
    RMCommandBuffer *cb = _cb;
    [cb record:cmd(OpEnd)];
    cb->_active = nil;
    id<MTLComputeCommandEncoder> c = [(id<MTLCommandBuffer>)cb computeCommandEncoder];
    [c setComputePipelineState:state];
    [c setBuffer:(id<MTLBuffer>)b->_storage offset:range.location * raw::counters::kRecordBytes atIndex:0];
    [c setBuffer:destination offset:offset - shift atIndex:1];
    if (shift)
        [c setBytes:&shifted length:sizeof shifted atIndex:2];
    else
        [c setBytes:&params length:sizeof params atIndex:2];
    // read where the GPU runs the resolve, so a power-up between this encode and the samples has its anchor
    if (anchors.va) {
        Cmd m = cmd(OpKernelClockAnchors, 3, 3);
        m.c = anchors.va;
        m.b = sizeof(TegraGM20BAbiClockAnchors);
        [cb record:m];
    } else {
        const TegraGM20BAbiClockAnchors none = {};
        [c setBytes:&none length:sizeof none atIndex:3];
    }
    const NSUInteger groups = (range.length + raw::counters::kResolveThreads - 1) / raw::counters::kResolveThreads;
    [c dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(raw::counters::kResolveThreads, 1, 1)];
    [c endEncoding];
    [cb begin:self];
    [cb record:cmd(OpBlit)];
}

@end

@interface RMEncoder (Counters)
@end
@implementation RMEncoder (Counters)

// The driver reports it unsupported (MTLReportFailure) on every encoder: supportsCounterSampling: answers NO for draw,
// dispatch and blit boundaries, and an M4 Max's driver aborts the process
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)index
                   withBarrier:(BOOL)barrier
{
    (void)sampleBuffer, (void)barrier;
    rmCallInvalid(_cb, PredCallCounter, index, 0,
                  "sampleCountersInBuffer:atSampleIndex:withBarrier: on an encoder, a sampling point this device has "
                  "none of");
}

@end
