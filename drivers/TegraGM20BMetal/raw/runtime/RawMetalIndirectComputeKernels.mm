// SPDX-License-Identifier: MIT
// An indirect dispatch command's kernels at its shape, and its regions and binds written into its block
// (RawMetalIndirectCompute.h)
#import "RawMetalIndirectCompute.h"
#include "RawDirect.h"
#include "raw_compute_key.h"

using RMKernelJob = raw::ComputeKernelJob<raw::ComputeState>;

namespace {

void waitFor(RMKernelJob &job)
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    if (job.observe([done] {
            dispatch_semaphore_signal(done);
        }) == RMKernelJob::State::Pending)
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}

std::shared_ptr<RMKernelJob> requestKernel(RMComputeState *state, const raw::ComputeSpecialization &specialization)
{
    return state->_kernels.request(specialization, [&](std::shared_ptr<RMKernelJob> pending) {
        RMKernelJob::Run run{pending};
        raw::Completion *completion = gRM->completion.get();
        if (!completion)
            return;
        const std::map<uint32_t, uint32_t> memory(specialization.memory.begin(), specialization.memory.end());
        raw::KernelDispatch ask(specialization.threads.data(), memory, specialization.regionMode);
        const auto text = specialization.text();
        const auto key = raw::computeKey(state->_air, state->_constants, state->_maxThreads, state->_multipleOfWidth,
                                         specialization.threads.data(), text.memory, text.textures, text.samplers,
                                         specialization.regionMode);
        run.result.state = raw::computeState(*gRM->raw, raw::computeKeyHash(key));
        if (run.result.state) {
            run.result.retry = false;
            return;
        }
        run.result.state = completion->kernel(state->_direct, ask.dispatch(), key, &run.result.retry, state->_function);
    });
}

// A region's launches: its slices within the QMD's grid fields
uint64_t regionLaunches(const raw::ComputeRegion &region)
{
    return raw::qmdSliceCount({region.groups[0], region.groups[1], region.groups[2]});
}

}  // namespace

std::shared_ptr<RMKernelSet> rmCommandKernels(RMComputeState *pipeline, const RMICBDispatch &c,
                                              raw::ComputeDispatch &shape, RMICBUnmade &unmade)
{
    unmade = {};
    if (!c.dispatched)
        return nullptr;
    if (!pipeline) {
        unmade = {"an indirect dispatch command with no compute pipeline state of this device",
                  raw::ComputeFailure::Invalid};
        return nullptr;
    }
    shape = raw::computeDispatch(c.grid, c.threads, c.perThread, pipeline->_threadLimit);
    if (shape.reason != raw::ComputeDispatch::Ready) {
        unmade = {"an indirect dispatch command's threads per threadgroup past its state's limit or none on an axis, "
                  "or a grid past a thread's position",
                  raw::ComputeFailure::Invalid};
        return nullptr;
    }
    if (!shape.count)
        return nullptr;
    auto kernels = std::make_shared<RMKernelSet>();
    kernels->count = shape.count;
    for (uint8_t region = 0; region < shape.count; ++region) {
        raw::ComputeSpecialization specialization;
        specialization.threads = shape.regions[region].local;
        specialization.regionMode = shape.multi;
        specialization.memory = c.memory;
        kernels->jobs[region] = requestKernel(pipeline, specialization);
        waitFor(*kernels->jobs[region]);
    }
    if (kernels->state() != RMKernelSet::State::Ready) {
        unmade = {"an indirect dispatch command whose kernel the shader compiler refused, or could not make"};
        return nullptr;
    }
    for (uint8_t region = 0; region < kernels->count; ++region)
        if (!raw::ComputeEncoder::icbArgumentTables(*gRM->raw, *kernels->jobs[region]->value())) {
            unmade = {"no memory for the tables an indirect dispatch command's kernel reads argument buffers through",
                      raw::ComputeFailure::OutOfMemory};
            return nullptr;
        }
    return kernels;
}

uint64_t rmCommandBytes(const RMKernelSet &kernels, const raw::ComputeDispatch &shape)
{
    uint64_t bytes = 0;
    for (uint8_t region = 0; region < kernels.count; ++region)
        bytes += uint64_t(raw::ComputeEncoder::icbRegionBytes(*kernels.jobs[region]->value()->rec)) *
                 regionLaunches(shape.regions[region]);
    return bytes;
}

void rmCommandBuffers(const RMICBDispatch &c, raw::ComputeEncoder::IcbBuffer (&out)[31])
{
    for (uint32_t slot = 0; slot < 31; slot++) {
        RMBuffer *b = c.buffers[slot];
        out[slot] = {};
        if (b && b->_raw.va && c.offsets[slot] <= b->_raw.length)
            out[slot] = {b->_raw.va + c.offsets[slot],
                         uint32_t(std::min<uint64_t>(b->_raw.length - c.offsets[slot], 0xffffffffu))};
    }
}

bool rmCommandBinds(const RMKernelSet &kernels, const raw::ComputeDispatch &shape, const RMICBBlock &block,
                    RMICBDispatch &c, const char **why)
{
    c.binds.clear();
    c.strideWords.clear();
    c.roots.clear();
    c.bindSlots = c.strideSlots = 0;
    uint64_t at = 0;
    for (uint8_t region = 0; region < kernels.count; ++region) {
        const raw::ComputeState &k = *kernels.jobs[region]->value();
        for (uint64_t s = 0; s < regionLaunches(shape.regions[region]); ++s) {
            if (!raw::ComputeEncoder::icbBinds(k, block.va + at, c.binds, c.strideWords, c.roots, why))
                return false;
            at += raw::ComputeEncoder::icbRegionBytes(*k.rec);
        }
    }
    for (const raw::icb::ComputeBind &b : c.binds)
        c.bindSlots |= 1u << b.slot;
    for (const raw::icb::ComputeStride &s : c.strideWords)
        c.strideSlots |= 1u << s.slot;
    for (const raw::icb::ComputeRoot &r : c.roots)
        c.bindSlots |= 1u << r.slot;
    return true;
}

bool rmCommandRegions(const RMKernelSet &kernels, const raw::ComputeDispatch &shape, const RMICBDispatch &c,
                      const raw::ComputeEncoder::IcbBuffer (&buffers)[31], const std::array<uint32_t, 31> &strides,
                      const RMICBBlock &block, raw::icb::ComputeCommand &words, const char **why)
{
    raw::StageIn stageIn;
    stageIn.strides = strides;
    if (c.stageIn)
        stageIn.setRegion({c.stageInRegion.origin.x, c.stageInRegion.origin.y, c.stageInRegion.origin.z,
                           c.stageInRegion.size.width, c.stageInRegion.size.height, c.stageInRegion.size.depth});
    uint64_t at = 0;
    words.qmd.clear();
    for (uint8_t region = 0; region < kernels.count; ++region) {
        const raw::ComputeState &k = *kernels.jobs[region]->value();
        const raw::ComputeRegion &part = shape.regions[region];
        for (uint64_t s = 0; s < regionLaunches(part); ++s) {
            if (!raw::ComputeEncoder::icbRegion(*gRM->raw, k, part, raw::qmdSlice({{}, part.groups}, s), shape.grid,
                                                shape.nominal, buffers, stageIn, block.cpu + at, block.va + at, why))
                return false;
            words.qmd.push_back(block.va + at);
            at += raw::ComputeEncoder::icbRegionBytes(*k.rec);
        }
    }
    return true;
}
