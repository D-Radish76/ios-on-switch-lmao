// SPDX-License-Identifier: MIT
#import "RawMetalRangeEncode.h"
#import "RawMetalPipeline.h"
#include "raw_lock.h"

// ---- the tape onto the raw path ----

RMRangeEncode &rmRangeEncode()
{
    static RMRangeEncode *encode = new RMRangeEncode;
    return *encode;
}

RMPred refusalPred(uint8_t id)
{
    static const RMPred map[] = {PredEncPassRender, PredEncIndexedRange, PredEncColorFormats, PredEncNoTic,
                                 PredEncDepthStencil, PredEncSamples, PredEncGeometry, PredEncStorageView,
                                 PredPassRotated, PredEncPassRender, PredEncPassRender};
    static_assert(sizeof map / sizeof *map == raw::RefWrittenTexture + 1, "a predicate for each refusal");
    return id < sizeof map / sizeof *map ? map[id] : PredEncPassRender;
}

// Encoded ahead, a failure is not the command buffer's yet: its encode at the commit meets it again and counts it. A draw
// that refused under another step (a turned pass's reach) still fails as Metal's error.
bool RMRangeEncode::fail(RMPred p, const char *w, uint64_t a, uint64_t b, const char *detail)
{
    *why_ = w;
    if (ahead_)
        veto_ = VetoFailed;
    else {
        nameFailure(cb_, p);
        if (!rc_->refusal.id || !drawFailed(cb_, rc_->refusal, w))
            hit(cb_, p, a, b, detail);
    }
    rc_->abandon();
    abandoned_ = true;
    return false;
}

bool RMRangeEncode::stop(uint32_t veto)
{
    veto_ = veto;
    return false;
}

// A pass raw::CommandBuffer::render refused: Metal's validation or out of memory, named in the predicate's first line
bool RMRangeEncode::passRefused(uint64_t a)
{
    if (!ahead_ && !predLogged(PredEncPassRender))
        logPred(PredEncPassRender, a, 0, @(*why_));
    return fail(PredEncPassRender, *why_, a);
}

// A draw the encoder refused: Metal's error, or its predicate and values (raw_gpu.h Refusal)
bool RMRangeEncode::refused()
{
    const raw::Refusal &f = rc_->refusal;
    if (rc_->contentsDeferred)
        veto_ = VetoContents;
    else if (ahead_)
        veto_ = VetoFailed;
    else {
        nameFailure(cb_, refusalPred(f.id));
        if (!drawFailed(cb_, f, *why_))
            hit(cb_, refusalPred(f.id), f.a, uint64_t(f.b) | uint64_t(f.c) << 32, *why_);
    }
    rc_->abandon();
    abandoned_ = true;
    return false;
}

__attribute__((objc_externally_retained)) bool RMRangeEncode::open(RMCommandBuffer *cb, size_t from, size_t to,
                                                                   bool whole, const char **why)
{
    const Encoding encoding;
    cb_ = cb;
    d_ = gRM->raw;
    why_ = why;
    from_ = at_ = from;
    to_ = to;
    whole_ = whole;
    ahead_ = !whole;
    abandoned_ = false;
    veto_ = 0;
    sampledShadows_ = whole && cb->_sampledShadows;
    standIns_ = whole && cb->_standIns;
    raw::Device &d = *d_;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (whole)
        d.nextForms = cb->_probeForms;
#endif
    rc_ = d.begin();
    if (!rc_) {
        *why = "the kernel gave the raw path no channel";
        if (ahead_)
            return stop(VetoChannel);
        hit(cb, PredEncNoChannel, 0, 0, d.channelError().c_str());
        return false;
    }
    generation_ = d.channelGeneration();
    rc_->internalCompletion();
    beginInlineSignals(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    rc_->forceWholePrograms(cb->_forceWholePrograms);
    rc_->inlineStates(cb->_inlineStates);
#endif
    rc_->deferContents = ahead_;
    if (whole && !producersOrdered())
        return false;
    if (whole)
        firstContents_.emplace(cb, from, to, *rc_);
    else
        firstContents_.emplace();
    RAW_ENCODE_BEGIN(resourceSpan, Resources);
    if (whole) {
        resources(from, to);
        counters_.emplace(cb, from, to, *rc_);
    } else {
        counters_.emplace(*rc_);
    }
    // A range that starts inside a compute encoder (a loop's continuation, or what follows a block that holds loops:
    // RMSegment::held): that encoder's work goes on in a new raw encoder, the planned state applied at its first dispatch
    for (size_t at = from; at-- > 0;) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpCompute) {
            ce_.emplace(*rc_, d);
            serialCompute_ = c.a == MTLDispatchTypeSerial;
            ce_->setSerial(serialCompute_);
            computeUnit_ = true;
        }
        if (c.op == OpCompute || c.op == OpRender || c.op == OpBlit || c.op == OpEnd)
            break;
    }
    if (whole && cb->_borderSamplers && !borderHeaders(from, to))
        return false;
    if (whole && !conversions())
        return false;
    RAW_ENCODE_END(resourceSpan);
    return true;
}

// The resources [from, to) names: their pitch imports, their CPU lines cleaned, each named to the job
bool RMRangeEncode::resources(size_t from, size_t to)
{
    raw::CommandBuffer *rc = rc_;
    const auto buffers = [&](auto visit) {
        cb_->_plan.forEachResource(from, to, RMResourceKind::Buffer, visit);
    };
    const auto textures = [&](auto visit) {
        cb_->_plan.forEachResource(from, to, RMResourceKind::Texture, visit);
    };
    const auto pitchBuffer = [&](const RMRangeResource &use) {
        if (__unsafe_unretained RMBuffer *buffer = (__bridge RMBuffer *)use.facts.cpuBuffer)
            rc->requirePitch(buffer->_raw.import, use.access & raw::ResourceUse::Write);
    };
    buffers(pitchBuffer);
    textures([&](const RMRangeResource &use) {
        pitchBuffer(use);
        __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)use.object;
        const bool writes = use.access & raw::ResourceUse::Write;
        if (writes || (use.access & raw::ResourceUse::Explicit) || !(use.access & raw::ResourceUse::Sample)) {
            rc->requirePitch(texture->_raw.import, writes);
            for (const raw::Texture *plane : {texture->_raw.luma, texture->_raw.chroma})
                if (plane)
                    rc->requirePitch(plane->import, writes);
        }
    });
    textures([&](const RMRangeResource &use) {
        if (!(use.access & raw::ResourceUse::Sample))
            return;
        __unsafe_unretained RMTexture *texture = (__bridge RMTexture *)use.object;
        for (const raw::Texture *plane : {&texture->_raw, texture->_raw.luma, texture->_raw.chroma})
            if (plane && plane->nativePlane != ~0u && rc->readTexture(plane) == plane)
                rc->requirePitch(plane->import, false);
    });
    const auto cleanBuffer = [&](const RMRangeResource &use) {
        const bool write = use.access & raw::ResourceUse::Write;
        if (__unsafe_unretained RMBuffer *buffer = (__bridge RMBuffer *)use.facts.cpuBuffer;
            buffer && buffer->_raw.hostVisible && !buffer->_raw.wc &&
            (write || (use.access & raw::ResourceUse::Explicit)))
            rc->clean(buffer->_raw.cpu, buffer->_raw.length, write);
    };
    buffers([&](const RMRangeResource &use) {
        cleanBuffer(use);
        useBuffer(rc, (__bridge RMBuffer *)use.object, use.access & raw::ResourceUse::Write);
    });
    textures([&](const RMRangeResource &use) {
        cleanBuffer(use);
        useTexture(rc, (__bridge RMTexture *)use.object, use.access & raw::ResourceUse::Write);
    });
    cb_->_plan.forEachResource(from, to, RMResourceKind::Heap, [&](const RMRangeResource &use) {
        if (__unsafe_unretained RMHeap *heap = (__bridge RMHeap *)use.object; heap->_members)
            rc->useHeap(*heap->_members);
    });
    return true;
}

// opaque borders' headers for the textures [from, to) samples (Sampler::bordered), which the draws' handles take. With no
// entry left in the sampler header table the command buffer fails OutOfMemory; encoded ahead, that is the commit's encode's
// to meet (fail's veto).
bool RMRangeEncode::borderHeaders(size_t from, size_t to)
{
    bordered_.clear();
    cb_->_plan.forEachResource(from, to, RMResourceKind::Sampler, [&](const RMRangeResource &use) {
        if (RMSampler *s = (__bridge RMSampler *)use.object; s->_raw.desc.border)
            bordered_.push_back(s);
    });
    if (bordered_.empty())
        return true;
    bool made = true;
    cb_->_plan.forEachResource(from, to, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        if (use.access & raw::ResourceUse::Sample)
            for (__unsafe_unretained RMSampler *s : bordered_)
                made &= d_->borderVariant(s->_raw, ((__bridge RMTexture *)use.object)->_raw);
    });
    if (!made) {
        encoder_ = SIZE_MAX;
        if (ahead_)
            return fail(PredCallSamplerForeign, "no sampler header for an opaque border");
        hit(cb_, PredCallSamplerForeign, 0, 0, "no sampler header left in the device's table for an opaque border");
        failCommandBuffer(cb_, @"no sampler header left in the device's table for an opaque border",
                          MTLCommandBufferErrorOutOfMemory, PredCallSamplerForeign);
        rc_->abandon();
        abandoned_ = true;
        return false;
    }
    return true;
}

// What an encode ahead cannot take from [from, until): work it is not built for, or what only the commit makes final
uint32_t RMRangeEncode::ineligible(size_t from, size_t until)
{
    const RMCommandPlan &plan = cb_->_plan;
    for (size_t at = from; at < until; ++at) {
        const Op op = plan.operation(at).op;
        if (op == OpSignalEvent || op == OpWaitEvent || op == OpCompute || op == OpDispatch || isKernelCommand(op) ||
            op == OpExecuteCommands || op == OpDrawPatches || op == OpDrawPatchesIndirect ||
            op == OpPredicate || op == OpResetCommands || op == OpCopyCommands)
            return VetoOps;
        if (!RMCommandPlan::isEncoderBegin(op))
            continue;
        const size_t end = plan.encoderEnd(at);
        // its images are converted from their surfaces at the seeds the commit makes final (conversions)
        bool planar = false;
        plan.forEachResource(at, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
            planar |= (use.access & raw::ResourceUse::Sample) && (use.facts.roles & RMResourceFacts::Converted);
        });
        if (planar)
            return VetoPlanar;
        if (op == OpRender && loadsAStoredDepth(plan.legacy(at).x))
            return VetoZcull;
    }
    return 0;
}

// The pass at `x` loads a depth an earlier pass of the command buffer stores: whether that one keeps its zcull region
// (raw::DepthStencilAttachment::loadedLater) is read from the passes after it, which only the whole range's encode has
bool RMRangeEncode::loadsAStoredDepth(size_t x)
{
    const RMPass *passes = cb_->_passes.data();
    const RMPass &pass = passes[x];
    return pass.depth && pass.depthLoad == MTLLoadActionLoad &&
           std::any_of(passes, passes + x, [&](const RMPass &earlier) {
               return earlier.depth == pass.depth && (earlier.zeta[0].store != MTLStoreActionDontCare ||
                                                      earlier.zeta[1].store != MTLStoreActionDontCare);
           });
}

// Encoded ahead, the encoder at `begin` is entered: its resources named, its opaque borders' sampler headers made, the
// fresh textures it uses first written ahead of it (where the end of the encoder before it did not). A surface it writes
// that an encoder before it read through the native image stops it: the whole range's encode names the pitch view to every
// encoder.
bool RMRangeEncode::enter(size_t begin)
{
    const size_t end = cb_->_plan.encoderEnd(begin);
    resources(begin, end);
    if (rc_->pitchAfterNativeRead()) {
        rc_->abandon();
        abandoned_ = true;
        return stop(VetoNative);
    }
    if (!borderHeaders(begin, end))
        return false;
    if (freshAt_ != begin) {
        freshAt_ = begin;
        if (firstContents_->claim(cb_, begin, end))
            firstContents_->written(*rc_);
    }
    return true;
}

bool RMRangeEncode::step(size_t until)
{
    const Encoding encoding;
    if (!whole_)
        if (const uint32_t veto = ineligible(at_, until)) {
            rc_->abandon();
            abandoned_ = true;
            return stop(veto);
        }
    RAW_ENCODE_BEGIN(loopSpan, Loop);
    for (; at_ < until; ++at_) {
        if (!whole_ && RMCommandPlan::isEncoderBegin(cb_->_plan.operation(at_).op) && !enter(at_))
            return false;
        if (!command(at_))
            return false;
        if (rc_->contentsDeferred) {
            rc_->abandon();
            abandoned_ = true;
            return stop(VetoContents);
        }
        if (!rewritten_.empty() && !continuing_ && cb_->_plan.operation(at_).op == OpEnd && !remakeImages(at_ + 1))
            return false;
    }
    RAW_ENCODE_END(loopSpan);
    return true;
}

// The range's GPU event waits are on producers this channel orders, or that succeeded. False: one failed (the command
// buffer's error), or one is pending on a channel made again since (producerPending())
bool RMRangeEncode::producersOrdered()
{
    const raw::EventDependencyState producers = validateNativeEventWaits(cb_, generation_);
    if (producers == raw::EventDependencyState::ready)
        return true;
    rc_->abandon();
    abandoned_ = true;
    producerPending_ = producers == raw::EventDependencyState::blocked;
    *why_ = "an event producer failed or is pending on a channel made again since";
    return false;
}

bool RMRangeEncode::resume(size_t to, const char **why)
{
    why_ = why;
    to_ = to;
    ahead_ = false;
    rc_->deferContents = false;
    sampledShadows_ = cb_->_sampledShadows;
    standIns_ = cb_->_standIns;
    if (!producersOrdered())
        return false;
    // a surface it read through its native view before the commit, published or written since: the whole encode reads it
    // again
    if (!rc_->nativeReadsCurrent()) {
        rc_->abandon();
        abandoned_ = true;
        return stop(VetoNative);
    }
    return true;
}

void RMRangeEncode::reset()
{
    if (rc_ && !abandoned_ && !sealed_)
        rc_->abandon();
    sealed_ = abandoned_ = ahead_ = producerPending_ = false;
    veto_ = 0;
    firstContents_.reset();
    counters_.reset();
    ce_.reset();
    re_ = nullptr;
    be_ = nullptr;
    rc_ = nullptr;
    cb_ = nil;
    serialCompute_ = computeUnit_ = continuing_ = encoderShadows_ = false;
    kernelShadows_ = {};
    stateCursor_ = {};
    fused_ = {};
    fusedAt_ = renderBegin_ = encoder_ = freshAt_ = SIZE_MAX;
    planned_ = {};
    standInLoads_.clear();
    planesWritten_.clear();
    overlapStores_.clear();
    overlapLoaded_.clear();
    standInStores_.clear();
    backings_ = {};
    planeLoads_.clear();
    loadedReach_ = {};
    loadedTargets_ = 0;
    formatTargets_.clear();
    formatStores_.clear();
    planeImages_.clear();
    resolves_.clear();
    resolveTargets_.clear();
    turned_ = {};
    depthResolve_ = {};
    stencilResolve_ = {};
    converted_.clear();
    rewritten_.clear();
    fresh_.clear();
    for (raw::Texture &t : levelTargets_)
        t = {};
    zetaPart_ = {};
    separatePart_[0] = separatePart_[1] = {};
    separateResolved_[0] = separateResolved_[1] = nullptr;
    depth16Scratch_ = nullptr;
    shadowsWritten_.clear();
    standInsWritten_.clear();
    planesImaged_.clear();
    morePlanes_.clear();
    standInFresh_.clear();
    codes_.clear();
    codeParts_.clear();
#if SWITCH_RAW_DIAGNOSTICS
    levelPasses_ = 0;
#endif
}

RMEncoded encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why)
{
    RMRangeEncode &encode = rmRangeEncode();
    // what was encoded ahead of the commit goes on: the rest of the range, then the seal; one that stopped submitted
    // nothing, and the range is encoded whole
    if (rmPipelineTake(cb, from)) {
        cb->_xrShadowCurrent = nullptr;
        const bool encoded = encode.resume(to, why) && encode.step(to) && encode.seal(to);
        const bool stopped = !encoded && encode.vetoed();
        const bool waits = encode.producerPending();
        encode.reset();
        if (encoded) {
            rmPipelineSealed(cb, to);
            return RMEncoded::submitted;
        }
        cb->_released.clear();
        if (!stopped)
            return waits ? RMEncoded::producerPending : RMEncoded::stopped;
    }
    cb->_xrShadowCurrent = nullptr;
    const bool encoded = encode.open(cb, from, to, true, why) && encode.step(to) && encode.seal(to);
    const bool waits = encode.producerPending();
    encode.reset();
    if (!encoded) {
        cb->_released.clear();
        return waits ? RMEncoded::producerPending : RMEncoded::stopped;
    }
    rmPipelineSealed(cb, to);
    return RMEncoded::submitted;
}
