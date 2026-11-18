// SPDX-License-Identifier: MIT
// The range encode's encoders as they end, the stand-in stores after them (the resolves: RawMetalRangeResolve.mm), and the
// range's commit
#import "RawMetalRangeEncode.h"
#import "RawMetalPassErrors.h"
#import "RawMetalPassFusion.h"
#import "RawMetalSplitPlan.h"
#import "RawMetalTargetViews.h"
#include "raw_xr_pools.h"

#include <algorithm>

// The pass's backings of its YCbCr targets' stand-ins, read before the resolve passes take the encoder over, then the
// stand-ins stored into the planes
void RMRangeEncode::standInBackings()
{
    backings_ = {};
    for (const auto &[t, j] : standInStores_)
        if (t->_raw.ycbcrStore && re_)
            backings_[j] = re_->target(j);
}

bool RMRangeEncode::storeStandIns()
{
    for (const auto &[t, j] : standInStores_) {
        if (!t->_raw.ycbcrStore) {
            if (standInPass(t, j, true))
                return false;
            continue;
        }
        if (!backings_[j]) {
            constexpr const char *kNoBacking = "a YCbCr texture's pass left no stand-in backing";
            rmPassWorkFails(cb_, ahead_, kNoBacking, MTLCommandBufferErrorInternal);
            return fail(PredEncPlanarRT, kNoBacking, j);
        }
        const raw::PassReach *reach = loadedTargets_ >> j & 1 ? &loadedReach_ : nullptr;
        if (const int failed = storeYcbcrPlanes(rc_, t->_raw, *backings_[j], why_, reach)) {
            if (failed == 2) {
                rmOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
                return refused();
            }
            rmPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorOutOfMemory);
            return fail(PredEncPlanarRT, *why_, j);
        }
        planesWritten_.push_back(t);
    }
    standInStores_.clear();
    loadedTargets_ = 0;
    return true;
}

bool RMRangeEncode::storeOverlappingPlanes()
{
    // a pass that loads its planes and draws nothing stores nothing (an M4 Max over planes it reads and stores at other
    // rows: m2v-textures12 probes/f2/targets-full.txt #168, #169)
    for (RMTexture *t : overlapStores_) {
        if (!passDrew_ && std::find(overlapLoaded_.begin(), overlapLoaded_.end(), t) != overlapLoaded_.end())
            continue;
        if (const int failed = raw::storeXrPlaneRows(*d_, *rc_, t->_raw, why_)) {
            if (failed == 2) {
                rmOwnDrawFails(cb_, ahead_, *why_, rc_->refusal);
                return refused();
            }
            rmPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
            return fail(PredEncPlanarRT, *why_);
        }
    }
    overlapStores_.clear();
    overlapLoaded_.clear();
    return true;
}

// A pass fused into the open one (beginPass's continuing_) draws on the targets that one's begin laid out: a colour
// its draws bind as storage takes its backing's compression from that use, and one bound as a 2D array a block-linear
// backing (raw::CommandBuffer::render; a pitch target is no array). The pass after `open` binds no colour so that
// `open` did not.
static bool storageLaidOut(RMCommandBuffer *cb, size_t open, size_t next)
{
    raw::PassDesc a, b;
    passStorageMasks(cb, next, b);
    if (!b.storageMask)
        return true;
    passStorageMasks(cb, open, a);
    return !(b.storageMask & ~a.storageMask) && !(b.arrayedStorageMask & ~a.arrayedStorageMask);
}

// A Depth16Unorm a draw of the pass after `open` tests in its shader is the converted zeta only where `open` began it so
// (RawMetalRangeForms.mm depth16Tested)
static bool testedLaidOut(RMCommandBuffer *cb, size_t open, size_t next)
{
    return !cb->_passes[cb->_plan.legacy(next).x].shaderTestsDepth ||
           cb->_passes[cb->_plan.legacy(open).x].shaderTestsDepth;
}

bool RMRangeEncode::endEncoder(size_t at)
{
    RMCommandBuffer *cb = cb_;
    if (re_)
        re_->finishVisibility();
    // encoded ahead, the fresh textures the next encoder uses first are written between the two, so it fuses with none;
    // what follows an end there and begins no encoder was recorded on the ended one, which its commit moves inside it
    bool fresh = false;
    if (!whole_ && at + 1 < to_) {
        if (!RMCommandPlan::isEncoderBegin(cb->_plan.operation(at + 1).op)) {
            rc_->abandon();
            abandoned_ = true;
            return stop(VetoOps);
        }
        freshAt_ = at + 1;
        fresh = firstContents_->claim(cb, at + 1, cb->_plan.encoderEnd(at + 1));
    }
    if (re_ && !fresh && at + 1 < to_ && cb->_plan.operation(at + 1).op == OpRender) {
        if (rmContinuesPass(cb, at + 1)) {
            continuing_ = true;
            return true;
        }
        if (!re_->turned() && !rmGroupedPass(cb, renderBegin_) && !rmGroupedPass(cb, at + 1) && !counters_->at(at) &&
            !counters_->at(at + 1)) {
            // the next pass's facts are the next boundary's first pass's
            const bool shadows = sampledShadows_ || standIns_;
            const raw::FusionPass first = fusedAt_ == renderBegin_ ? fused_ : fusionPass(cb, renderBegin_, shadows);
            fused_ = fusionPass(cb, at + 1, shadows);
            fusedAt_ = at + 1;
            if (raw::canFusePasses(first, fused_) && storageLaidOut(cb, renderBegin_, at + 1) &&
                testedLaidOut(cb, renderBegin_, at + 1)) {
                continuing_ = true;
                return true;
            }
        }
    }
    if (!endEncoders())
        return false;
    if (fresh)
        firstContents_->written(*rc_);
    counters_->ended(at);
    re_ = nullptr;
    be_ = nullptr;
    cb->_xrShadowCurrent = nullptr;
    return true;
}

// The open encoders ended at an encoder's end, the render encoder told what its recording read first
bool RMRangeEncode::endEncoders()
{
    if (re_)
        re_->plan(planned_);
    return closeEncoders();
}

// The open encoders ended, their resolves and stand-ins after them
bool RMRangeEncode::closeEncoders()
{
    passDrew_ = rc_->draws != passDraws_;
    if (re_)
        re_->end();
    standInBackings();
    if (re_ && !resolvePasses())
        return false;
    if (be_)
        be_->end();
    if (ce_) {
        ce_->end();
        ce_.reset();
        endKernelShadows();
    }
    if (!turned_.end(*rc_, why_)) {
        rmPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
        return fail(PredPassRotated, *why_);
    }
    if (!storeStandIns() || !storeOverlappingPlanes() || !storeFormatTargets())
        return false;
    planesBack();
    return depth16Back() && writtenBack();
}

bool RMRangeEncode::seal(size_t to)
{
    const Encoding encoding;
    RMCommandBuffer *cb = cb_;
    raw::CommandBuffer *rc = rc_;
    to_ = to;
    sealed_ = true;
    RAW_ENCODE_SPAN(Tail);
    if (!closeEncoders())
        return false;
    encoder_ = SIZE_MAX;
    uint32_t seq = 0;
    prepareSharedEvents(cb, rc);
    rc->quietEnd = cb->_rangeQuiet;
    const bool lastRange = cb->_segNext + 1 == cb->_segs.size() && cb->_promised.load(std::memory_order_acquire);
    if (lastRange) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        cb->_promisesTaken = true;
        rc->promises = cb->_promises;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    splitCaptureCommitted(*rc);
#endif
    const bool committed = rc->commit(seq);
    cb->_rangeQuiet = rc->quietEnd;
    if (!committed) {
        if (rc->admissionBlocked) {
            RMQueue *queue = cb->_queue;
            int armed = KERN_SUCCESS;
            cb->_resourceReady = rc->resourceReadiness(
                [queue] {
                    wakeSubmit((__bridge void *)queue);
                },
                armed);
            if (!cb->_resourceReady) {
                std::lock_guard<raw::Mutex> lock(cb->_lock);
                cb->_segError = [NSError
                    errorWithDomain:MTLCommandBufferErrorDomain
                               code:MTLCommandBufferErrorInternal
                           userInfo:@{
                               NSLocalizedDescriptionKey : @"Resource readiness notification could not be armed"
                           }];
            }
            rc->abandon();
            return false;
        }
        if (!rc->setRefused.empty() && !rc->partial) {
            failCommandBuffer(cb, [NSString stringWithUTF8String:rc->setRefused.c_str()]);
            rc->abandon();
            return false;
        }
        // A view the kernel found stale stays stale for every later command buffer, and one whose seed did not move (another
        // process's GPU write of its surface) would be refused again
        if (rc->nativeError == kIOReturnNotReady)
            rc->invalidateNativeReads();
        if (rc->nativeError && !rc->partial)
            failCommandBuffer(cb,
                              rc->nativeError == kIOReturnNotReady
                                  ? @"A surface it reads changed while it was submitted"
                                  : @"Native ownership or event dependency failed",
                              MTLCommandBufferErrorInternal, PredEncKernel);
        char reason[192];
        rc->describeRefusal(reason, sizeof reason);
        // as Metal fails a command buffer its driver could not submit: nothing of it runs elsewhere
        if (!ahead_ && !rc->partial && !rc->nativeError)
            failCommandBuffer(
                cb, [NSString stringWithFormat:@"The GPU driver did not submit the command buffer: %s", reason],
                MTLCommandBufferErrorInternal);
        // a refusal after part of it ran: nothing of it may run again elsewhere
        if (!ahead_ && rc->partial)
            failCommandBuffer(cb, [NSString stringWithFormat:@"The GPU driver ran part of the command buffer and the "
                                                             @"kernel refused the rest: %s", reason],
                              MTLCommandBufferErrorInternal);
        return fail(PredEncKernel, "the kernel refused the submission", uint32_t(rc->kernelResult), 0, reason);
    }
    firstContents_->submitted(cb);
    if (rc->skips)
        skip(cb, refusalPred(rc->skipped.id), rc->skipped.a, uint64_t(rc->skipped.b) | uint64_t(rc->skipped.c) << 32,
             "a draw Apple's driver draws nothing defined for: skipped", rc->skips);
    publishSharedEvents(cb);
    if (rc->promisesKept) {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        cb->_promises.clear();
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb->_probeTaken.add(rc->probeTaken());
    noteReplayShape(cb, from_, to);
    d_->stats.slotsCompared += stateCursor_.slotsCompared;
    d_->stats.slotsInTables += stateCursor_.slotsInTables;
#endif
    for (const Conversion &c : converted_) {  // (submitted: their images hold the surfaces at these seeds)
        c.t->_imageSeed = c.seed;
        c.t->_imageWrites = c.writes;
        c.t->_imageNativeGeneration = c.nativeGeneration;
        c.t->_imageCurrent = true;
    }
    noteSurfaceWrites(cb, from_, to);
    for (__unsafe_unretained RMTexture *t :
         planesWritten_)  // a write the surface's seed does not show: converted again
        t->_imageCurrent = false;
    cb->_seqno = seq;
    cb->_channelGeneration = generation_;
    cb->_stamp = cb->_segs.size() == 1 && !cb->_segs.front().event ? rc->stamp : 0;
    cb->_rangeLoopStamp = rc->loopsLaid ? rc->stamp : 0;
    RAW_DIAGNOSTIC(d_->stats.levelPasses += levelPasses_);
    RAW_DIAGNOSTIC(gRM->stats.draws += rc->draws);
    RAW_DIAGNOSTIC(gRM->stats.passes += rc->passes);
    RAW_DIAGNOSTIC(gRM->stats.copies += rc->blits);
    return true;
}
