// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_scheduled_wait.h"
#include "TegraGM20BJobABI.h"
#import "RawMetalPipeline.h"

void wakeSubmit(void *queue, uint32_t dependencies)
{
    gRM->schedule.push({RMScheduleChange::Wake, nil, queue, dependencies});
}

// Its place in its queue's order is where the worker takes its enqueue, or its commit where that hands over first: one
// thread's in its call order, and another's after it where the app ordered the calls (the hand-off keeps each push behind
// those that happened before it).
void enqueueBuffer(RMCommandBuffer *cb)
{
    if (cb->_enqueued.exchange(true, std::memory_order_acq_rel))
        return;
    cb->_before = (__bridge_transfer RMCommandBuffer *)cb->_queue->_last.exchange((__bridge_retained void *)cb);
    MTLCommandBufferStatus none = MTLCommandBufferStatusNotEnqueued;
    cb->_status.compare_exchange_strong(none, MTLCommandBufferStatusEnqueued);
    gRM->schedule.push({RMScheduleChange::Enqueue, cb}, false);  // nothing to submit until its commit
}

// The imports of the IOSurfaces it writes. Only while the submit thread cannot take it: under the schedule lock, or on
// the submit thread.
static std::vector<uint64_t> writtenSurfaces(RMCommandBuffer *cb)
{
    std::vector<uint64_t> handles;
    cb->_plan.forEachWritten(0, cb->_plan.size(), [&](void *object) {
        id resource = (__bridge id)object;
        if (![resource isKindOfClass:[RMTexture class]])
            return;
        RMTexture *texture = (RMTexture *)resource;
        while (texture->_kind == RMTextureView && texture->_parent)
            texture = texture->_parent;
        const raw::Import *import = texture->_raw.import;
        if (texture->_surface && import && import->bo.handle &&
            std::find(handles.begin(), handles.end(), import->bo.handle) == handles.end())
            handles.push_back(import->bo.handle);
    });
    return handles;
}

// Before its waitUntilScheduled may return held (raw_scheduled_wait.h): its writes of IOSurfaces are promised, so a
// present or another process's job that uses one waits for its job on the GPU, as for a scheduled command buffer on
// Apple's GPU. Its last raw range's last job keeps the promises; one made once that range had gone is given back.
static void promiseWrites(RMCommandBuffer *cb, const std::vector<uint64_t> &handles)
{
    if (handles.empty())
        return;
    {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        if (cb->_promised.load(std::memory_order_relaxed) || cb->_promisesTaken)
            return;
        cb->_promised.store(true, std::memory_order_release);
    }
    std::vector<uint32_t> made;
    for (size_t at = 0; at < handles.size() && made.size() < TegraGM20BMaxJobPromises; at += TegraGM20BMaxPromiseUses) {
        const std::vector<uint64_t> part(handles.begin() + ptrdiff_t(at),
                                         handles.begin() +
                                             ptrdiff_t(std::min(handles.size(), at + TegraGM20BMaxPromiseUses)));
        uint32_t id = 0;
        if (gRM->raw->promise(part, id))
            made.push_back(id);
        else
            once(
                @"the kernel took no writer promise for surfaces of a command buffer held on a shared event: a present "
                @"may show them before it is submitted");
    }
    bool late;
    {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        late = cb->_promisesTaken;
        if (!late)
            cb->_promises = made;
    }
    if (late)
        for (uint32_t id : made)
            gRM->raw->cancelPromise(id);
}

// finish(): what its last raw range did not carry to the kernel, after all its work completed. One that failed before its
// last range was submitted never makes the IOSurface writes it was to make, and is settled where its failure is decided,
// before its queue goes on to a command buffer that may write the same surfaces: its promises are withdrawn, or with
// none its writes themselves, so a present of one of those surfaces is refused instead of showing the frame before.
// _promised marks it settled: a promise already holds presents off, and unpromised writes set it once withdrawn.
void cancelPromises(RMCommandBuffer *cb)
{
    const bool failed = cb->_segError.set() && !cb->_scheduledSent.load(std::memory_order_acquire);
    if (!failed && !cb->_promised.load(std::memory_order_acquire))
        return;
    std::vector<uint32_t> left;
    bool promised;
    {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        promised = cb->_promised.load(std::memory_order_relaxed);
        cb->_promisesTaken = true;
        left.swap(cb->_promises);
    }
    int refusal = KERN_SUCCESS;
    for (uint32_t id : left)
        if (const int kr = gRM->raw->cancelPromise(id, failed))
            refusal = kr;
    if (failed && !promised) {
        const std::vector<uint64_t> written = writtenSurfaces(cb);
        const int kr = written.empty() ? KERN_SUCCESS : gRM->raw->withdrawWrites(written);
        if (kr != KERN_SUCCESS)
            refusal = kr;
        cb->_promised.store(true, std::memory_order_release);
    }
    if (failed && refusal != KERN_SUCCESS)
        os_log_error(rawLog(),
                     "raw: the kernel refused to withdraw the IOSurface writes of a failed command buffer (0x%x): a "
                     "present of one may show the frame before",
                     unsigned(refusal));
}

// Method 34 Arm: the kernel holds the bound on its unkept writer promises too, since Metal's bound is its kernel's and
// fires whatever this process is doing, suspended or stopped; a reader of the surfaces then goes on at `due` without
// waiting for this worker to run. `fails`: the command buffer fails at `due` (an untimed wait's own bound), so the
// kernel refuses a later keep. On the worker: it arms before it keeps, on one thread.
static void armBound(RMCommandBuffer *cb, raw::EventDeadline::Time due, bool fails = false)
{
    uint32_t mask = 0;
    {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        for (uint32_t id : cb->_promises)
            mask |= 1u << id;
    }
    if (!mask || due == raw::EventDeadline::Time{})
        return;
    const int64_t left = std::chrono::duration_cast<std::chrono::nanoseconds>(due - raw::EventDeadline::now()).count();
    if (const int kr = gRM->raw->armPromises(mask, uint64_t(std::max<int64_t>(left, 0)), fails))
        once([NSString stringWithFormat:@"the kernel took no bound for the writer promises of a command buffer held on a "
                                        @"shared event (0x%x): a reader of its surfaces waits for this process to run",
                                        kr]);
}

// The head held at a wait on a shared event: its own bound (`fails`: an untimed wait's, where it fails), and one a bound
// later for every command buffer held behind it, which this worker runs once the head fails or goes on (on Apple's GPU
// a held buffer behind another fails 5 s after it)
static raw::EventDeadline::Time holdBound(RMCommandBuffer *cb, bool &fails)
{
    const Cmd c = cb->_plan.legacy(cb->_segs[cb->_segNext].from);
    const raw::EventDeadline wait(cb->_eventStart, uint32_t(c.a));
    fails = !c.a;
    cb->_queue->_heldBound = wait.holderBound() + raw::EventDeadline::kHeldBound;
    return wait.holderBound();
}

void commitBuffer(RMCommandBuffer *cb)
{
    raw::followers().committing();
    planSegments(cb);
    RawState &r = *gRM;
    r.committed.fetch_add(1);
    if (r.raw->purgedArenas())
        restoreArenas(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (waitsForEvent(cb)) {
        std::lock_guard<raw::Mutex> h(gHoldersLock);
        gHolders.insert(cb);
        cb->_inHolders.store(true, std::memory_order_release);
    }
#endif
    armEventWaits(cb);
    RAW_DIAGNOSTIC(r.stats.deferred++);
    r.schedule.push({RMScheduleChange::Commit, cb});
}

static thread_local bool tSubmitWorker = false;

bool rmOnSubmitWorker()
{
    return tSubmitWorker;
}

// The worker's, between its submissions: a move begins and commits the device's one command buffer on its own, so an
// encode ahead holding it goes back first
static void runMove(const std::function<void()> &work)
{
    rmPipelineYield(nil, SIZE_MAX, false);
    work();
}

void rmRunOnSubmitWorker(const std::function<void()> &work)
{
    if (tSubmitWorker) {
        runMove(work);
        return;
    }
    RMMove move{&work, dispatch_semaphore_create(0)};
    RMScheduleChange change;
    change.kind = RMScheduleChange::Move;
    change.move = &move;
    gRM->schedule.push(std::move(change));
    dispatch_semaphore_wait(move.done, DISPATCH_TIME_FOREVER);
}

// A change another thread handed the worker, on the worker
static void applyChange(RawState &r, const RMScheduleChange &change)
{
    RMCommandBuffer *cb = change.buffer;
    if (change.kind == RMScheduleChange::Encoded) {
        rmPipelineHanded(cb);
        return;
    }
    if (change.kind == RMScheduleChange::Move) {
        runMove(*change.move->work);
        dispatch_semaphore_signal(change.move->done);
        return;
    }
    // a commit on one thread can hand over before the enqueue another thread made of the same command buffer
    if (change.kind != RMScheduleChange::Wake && !cb->_reserved) {
        cb->_reservation = r.waiting.reserve((__bridge void *)cb->_queue, cb);
        cb->_reserved = true;
    }
    switch (change.kind) {
    case RMScheduleChange::Enqueue:
        break;
    case RMScheduleChange::Commit: {
        bool held = false;
        r.waiting.commit((__bridge void *)cb->_queue, cb, cb->_reservation, &held);
        if (held) {
            promiseWrites(cb, writtenSurfaces(cb));
            cb->_held = true;
            armBound(cb, cb->_queue->_heldBound);
            rmChanged(cb);
        }
        break;
    }
    case RMScheduleChange::Wake:
        if (change.queue)
            r.waiting.wake(change.queue);
        r.waiting.wakeDependencies(change.dependencies);
        break;
    case RMScheduleChange::Encoded:
    case RMScheduleChange::Move:
        break;
    }
}

static void applyChanges(RawState &r)
{
    while (RMScheduleChange *change = r.schedule.peek()) {
        applyChange(r, *change);
        r.schedule.pop();
    }
}

// A native variant this queue head waits for, made on the raw compiler's two bounded workers: refused, its command buffer
// fails. No deadline.
// Work the raw path cannot run: its command buffer fails, as Metal's does for a pipeline it could not compile, never handed
// to another device.
static NSError *refusedAtReadiness(const char *why)
{
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain
                               code:MTLCommandBufferErrorInternal
                           userInfo:@{NSLocalizedDescriptionKey : [NSString stringWithFormat:@"%s", why ?: ""]}];
}

static int variantReadiness(RMCommandBuffer *cb, const RMSegment &range, const char **why)
{
    using Variant = raw::Variant<raw::Pipeline>;
    bool pending = false;
    for (void *object : cb->_plan.renderVariants(range.from, range.to)) {
        auto *variant = static_cast<Variant *>(object);
        std::function<void()> observer;
        if (cb->_variantObserved.insert(object).second) {
            void *queue = (__bridge void *)cb->_queue;
            observer = [queue] {
                wakeSubmit(queue);
            };
        }
        const auto state = variant->observe(std::move(observer));
        if (state == Variant::State::Failed)
            return -2;
        if (state == Variant::State::Refused) {
            *why = "a render state variant the shader compiler refused";
            return -1;
        }
        if (state == Variant::State::Pending)
            pending = true;
    }
    return pending ? 0 : 1;
}

// What a head eventReadiness found blocked waits for: its own earlier work, a value a submitted command buffer promised, or a
// shared event value nothing reached yet
static raw::HeadWait eventWait(RMCommandBuffer *cb)
{
    {
        std::lock_guard<raw::Mutex> l(cb->_lock);
        if (cb->_finishState.pending() || cb->_resourceReady)
            return raw::HeadWait::signal;
    }
    if (cb->_segNext >= cb->_segs.size() || !cb->_segs[cb->_segNext].event)
        return raw::HeadWait::signal;
    const Cmd c = cb->_plan.legacy(cb->_segs[cb->_segNext].from);
    if (c.op != OpWaitEvent)
        return raw::HeadWait::signal;
    return promisedEvent(c.obj, c.c) ? raw::HeadWait::promised : raw::HeadWait::event;
}

// The command buffers a hold took promise their writes, and those behind the held head get their bound; `head`, the held
// queue's head, has its own, which the submit loop arms
static void markHeld(std::vector<RMCommandBuffer *> &buffers, bool held, RMCommandBuffer *head = nil)
{
    for (RMCommandBuffer *cb : buffers) {
        if (held)
            promiseWrites(cb, writtenSurfaces(cb));
        cb->_held = held;
        if (held && cb != head)
            armBound(cb, cb->_queue->_heldBound);
        if (held)
            rmChanged(cb);
    }
    buffers.clear();
}

// The submit thread has no more raw work in hand: the quiet ranges it left in flight wait for their own interrupt. Only
// this thread stores the promise (retireRawRange).
static void closePromise()
{
    if (!gRM->submitPromised.load(std::memory_order_relaxed))
        return;
    gRM->submitPromised.store(false, std::memory_order_release);
    gRM->inflight.wake();
}

void submitLoop()
{
    RawState &r = *gRM;
    tSubmitWorker = true;
    std::vector<RMCommandBuffer *> marked;
    for (;;) {
        @autoreleasepool {
            raw::SubmitQueue<void *, RMCommandBuffer *>::Ticket ticket;
            bool followed = false;
            for (;;) {
                applyChanges(r);
                r.waiting.expire(raw::EventDeadline::now());
                r.waitingCommitted.store(uint32_t(r.waiting.committed()), std::memory_order_relaxed);
                ticket = r.waiting.next();
                if (ticket) {
                    followed = r.waiting.ready() || r.waiting.behind(ticket);
                    break;
                }
                closePromise();
                if (r.orphanCheck.load(std::memory_order_relaxed) &&
                    r.orphanCheck.exchange(false, std::memory_order_acquire))
                    rmPipelineOrphans();
                // nothing to submit: what the command buffer being recorded has published, encoded ahead of its commit
                if (rmPipelineStep())
                    continue;
                r.progress.idle(r.waiting.parked() != 0);
                const auto due = r.waiting.deadline();
                dispatch_time_t until = DISPATCH_TIME_FOREVER;
                if (due != raw::EventDeadline::Time::max()) {
                    const auto left = due - raw::EventDeadline::now();
                    until = dispatch_time(
                        DISPATCH_TIME_NOW,
                        std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count()));
                }
                r.submitAsleep.store(true, std::memory_order_seq_cst);
                if (rmPipelinePublished()) {
                    r.submitAsleep.store(false, std::memory_order_relaxed);
                    continue;
                }
                r.schedule.sleep(until);
                r.submitAsleep.store(false, std::memory_order_relaxed);
            }
            const uint64_t judged = r.progress.visit();
            RMCommandBuffer *cb = ticket.value;
            auto deadline = raw::EventDeadline::Time::max();
            uint32_t dependencies = 0;
            bool blocked = false;
            raw::HeadWait wait = raw::HeadWait::signal;
            const auto failed = [cb]() -> NSError * {
                if (!cb->_segError.set())
                    return nil;
                std::lock_guard<raw::Mutex> l(cb->_lock);
                return cb->_segError;
            };
            NSError *error = failed();
            if (!error) {
                blocked = eventReadiness(cb, deadline) == raw::EventReadiness::blocked;
                if (blocked)
                    wait = eventWait(cb);
                error = failed();
            }
            if (!error && !blocked && cb->_segNext < cb->_segs.size()) {
                RMSegment &range = cb->_segs[cb->_segNext];
                cb->_plan.prepareRange(range.from, range.to);
                // the completion's _loopContinue is written before its range's completed() releases the count
                const bool loopRetiring = range.held && cb->_finishState.pending() != 0;
                if (loopRetiring) {
                    blocked = true;
                    dependencies = RMRawProgress;
                } else if (!range.event) {
                    const char *why = "";
                    int ready = variantReadiness(cb, range, &why);
                    if (ready > 0)
                        ready = computeReadiness(cb, range, &why);
                    if (ready > 0)
                        ready = rmXrShadowReadiness(cb, range.from, range.to);
                    if (ready == -2) {
                        error = [NSError
                            errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorInternal
                                   userInfo:@{NSLocalizedDescriptionKey : @"A render pipeline could not be compiled"}];
                    } else if (ready < 0) {
                        error = refusedAtReadiness(why);
                    } else if (!ready) {
                        blocked = true;
                        wait = raw::HeadWait::compile;
                    }
                }
            }
            if (blocked) {
                if (!r.progress.mayPark(judged)) {  // a completion came since it was judged
                    r.waiting.yield(ticket);
                    continue;
                }
                r.waiting.park(ticket, dependencies, deadline);
                if (!raw::holds(wait)) {
                    markHeld(marked, true);
                    continue;
                }
                r.waiting.hold(ticket.key, marked);
                bool fails = false;
                const auto bound = holdBound(cb, fails);
                markHeld(marked, true, cb);
                if (cb->_boundArmed != cb->_eventArmed) {
                    cb->_boundArmed = cb->_eventArmed;
                    armBound(cb, bound, fails);
                }
                continue;
            }
            bool submitted = true;
            if (error) {
                rmPipelineFailed(cb);
                finish(cb, error);
            } else {
                submitted = submitRangeExclusive(cb, followed);
            }
            r.waiting.release(ticket.key, marked);
            if (submitted)
                r.waiting.retire(ticket);
            else
                r.waiting.yield(ticket);
            markHeld(marked, false);
        }
    }
}
