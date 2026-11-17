// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "../SwitchDisplayRetirement.h"
#include "../../TegraDisplayFlip/TegraScanoutRing.h"

namespace {
void IOLog(const char *, ...)
{}

uint64_t mach_absolute_time()
{
    return 100;
}

struct OriginalLegacySwapRequest {
    uint64_t sequence;
};

struct Display {
    static constexpr int kNoBuffer = -1;

    TegraScanoutRing ring;

    struct PlaneSet {
        uint64_t seq = 0, pendingSeq = 0;
    } planeHeld, planeFencing;

    struct Latch {
        int armed = -1, superseded = -1;

        bool requested() const
        {
            return armed >= 0;
        }
    } planeLatch;

    std::array<PlaneSet, 4> planes = {};
    std::array<uint64_t, 4> entryPendingSeq = {}, entrySeq = {};
    switch_display::Retirement scanoutRetirement;
    bool latchRetire = true, planeHeldSet = false, planeFencePending = false, gpuScanout = false;
    const OriginalLegacySwapRequest *gpuRequest = nullptr;
    std::vector<OriginalLegacySwapRequest> queue;
    std::vector<switch_display::RetiredFrame> shown;
    std::vector<uint64_t> retired;
    uint64_t completed = 0;
    unsigned kicks = 0;
    uint64_t gpuSequence = 0, gpuLostFrames = 0;
    bool gpuRetired = false, transferred = false, frontRead = true;

    PlaneSet &planeSetOf(int id)
    {
        return planes.at(id);
    }

    const PlaneSet &planeSetOf(int id) const
    {
        return planes.at(id);
    }

    const OriginalLegacySwapRequest *pendingSwapRequest() const
    {
        return queue.empty() ? nullptr : &queue.front();
    }

    uint64_t seqOf(const OriginalLegacySwapRequest *request) const
    {
        return request->sequence;
    }

    bool retireSwap(const OriginalLegacySwapRequest *request, bool)
    {
        if (request != pendingSwapRequest())
            return false;
        retired.push_back(request->sequence);
        completed = request->sequence;
        queue.erase(queue.begin());
        return true;
    }

    void frameShown(uint64_t sequence, uint64_t at)
    {
        shown.push_back({sequence, at});
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

    bool planeRecovering = false, planeRecoveryStuck = false;

    bool planeRecoveryWaits() const
    {
        return planeRecovering && !planeRecoveryStuck;
    }

    void releaseIdleDirectReads()
    {
        if (ring.front == kNoBuffer)
            frontRead = false;
    }

    PlaneSet planeRecoveryTarget = {};

    struct Probe {
        bool on = false;

        bool active() const
        {
            return on;
        }
    } probe, planeProbe;

    bool kickDeferred = false, tapeSettle = false, probeRequest = false, hwSuspended = false, scanoutStopped = false;

    struct {
        bool wanted = false;
        PlaneSet held;
    } landscapeState;

#include "../SwitchRetireDisplay.h"
#include "../SwitchLostFrameDisplay.h"
};

TEST(DisplayRetirement, AcceptedProducerLossCannotReportAPresentBeforeScanoutStops)
{
    Display display;
    display.queue = {{2}};
    display.dropLostFrame(display.pendingSwapRequest(), 3);
    EXPECT_TRUE(display.shown.empty());
    EXPECT_TRUE(display.retired.empty());
    EXPECT_TRUE(display.frontRead);
    EXPECT_EQ(display.gpuLostFrames, 1u);
}

TEST(DisplayRetirement, AcceptedLossesCannotBeReportedAsAnEarlierFramesLatch)
{
    Display display;
    display.queue = {{1}};
    display.ring.pending = 1;
    display.entrySeq[1] = display.entryPendingSeq[1] = 1;
    for (const uint64_t sequence : {2, 3}) {
        display.queue.push_back({sequence});
        display.dropLostFrame(&display.queue.back(), 3);
    }
    const uint64_t sequence = display.entrySeq[1];
    display.ring.pending = -1;
    display.retireThrough(sequence, 100);
    ASSERT_EQ(display.shown.size(), 1u);
    EXPECT_EQ(display.shown[0].sequence, 1u);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1}));
    EXPECT_TRUE(display.frontRead);
}

TEST(DisplayRetirement, TwoAcceptedLossesCannotReturnTheScannedPageToAThreePagePool)
{
    Display display;
    const std::array<uint64_t, 3> pages = {1, 2, 3};
    display.completed = pages[0];
    display.ring.front = 0;
    for (unsigned page = 1; page != pages.size(); ++page) {
        display.queue.push_back({pages[page]});
        display.dropLostFrame(&display.queue.back(), 3);
    }
    const bool firstPageReusable = display.completed >= pages[0] + 1;
    EXPECT_FALSE(firstPageReusable && display.frontRead);
    EXPECT_TRUE(display.shown.empty());
}

TEST(DisplayRetirement, AStuckRecoverysOverlaySetsNoLongerHoldRetirement)
{
    Display display;
    display.queue = {{1}, {2}};
    display.planeLatch.armed = 0;
    display.planes[0] = {1, 1};
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeRecovering = display.planeRecoveryStuck = true;
    display.retireThrough(2, 110);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1, 2}));
}

TEST(DisplayRetirement, DeferredSwapGoesAgainAtAnIdleFrameEndAndOnlyThere)
{
    Display display;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 0u);
    display.kickDeferred = true;
    display.ring.pending = 0;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 0u);
    display.ring.pending = -1;
    display.planeLatch.armed = 1;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 0u);
    display.planeLatch.armed = -1;
    display.kickAtIdleFrameEnd();
    EXPECT_EQ(display.kicks, 1u);
    EXPECT_FALSE(display.kickDeferred);
}

TEST(DisplayRetirement, FirstUiLatchWaitsForPairedOverlayThenCompletesWithoutAnotherUiFrame)
{
    Display display;
    display.queue = {{1}};
    display.planeLatch.armed = 0;
    display.planes[0] = {1, 1};
    display.retireThrough(1, 100);
    EXPECT_TRUE(display.shown.empty());
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = -1;
    display.retireThrough(1, 110);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1}));
    ASSERT_EQ(display.shown.size(), 1u);
    EXPECT_EQ(display.shown[0].shownAt, 110u);
}

TEST(DisplayRetirement, EarlierUiDependencySurvivesNewerVideoOnlyLatch)
{
    Display display;
    display.queue = {{2}, {3}};
    display.ring.pending = 0;
    display.entrySeq[0] = display.entryPendingSeq[0] = 2;
    display.retireThrough(3, 100);
    EXPECT_TRUE(display.retired.empty());
    display.ring.pending = -1;
    display.retireThrough(2, 120);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{2, 3}));
    ASSERT_EQ(display.shown.size(), 1u);
    EXPECT_EQ(display.shown[0].sequence, 3u);
    EXPECT_EQ(display.shown[0].shownAt, 120u);
}

TEST(DisplayRetirement, NoChangeSwapExtendingCompletionDoesNotMoveOldestOverlayDependency)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.planeLatch.armed = 0;
    display.planes[0] = {3, 1};
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = -1;
    display.retireThrough(3, 110);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1, 2, 3}));
}

TEST(DisplayRetirement, NoChangeSwapExtendingUiRetainsOriginalPendingSequence)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.ring.pending = 0;
    display.entrySeq[0] = 3;
    display.entryPendingSeq[0] = 1;
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.ring.pending = -1;
    display.retireThrough(3, 110);
    EXPECT_EQ(display.retired.size(), 3u);
}

TEST(DisplayRetirement, NewerPendingFrameDoesNotDelayEarlierCompletedFrame)
{
    Display display;
    display.queue = {{1}, {2}};
    display.planeLatch.armed = 0;
    display.planes[0] = {2, 2};
    display.retireThrough(1, 100);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1}));
    ASSERT_EQ(display.queue.size(), 1u);
    EXPECT_EQ(display.queue[0].sequence, 2u);
}

TEST(DisplayRetirement, SupersededAndHeldWindowsRetainTheirResources)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.planeLatch.armed = 1;
    display.planeLatch.superseded = 0;
    display.planes[0] = {1, 1};
    display.planes[1] = {2, 2};
    display.planeHeldSet = true;
    display.planeHeld = {3, 3};
    display.retireThrough(3, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = display.planeLatch.superseded = -1;
    display.retireThrough(2, 110);
    EXPECT_TRUE(display.retired.empty());
    display.planeHeldSet = false;
    display.retireThrough(3, 120);
    EXPECT_EQ(display.retired.size(), 3u);
}

TEST(DisplayRetirement, PendingProducerFenceCannotBeRetiredByAnotherWindow)
{
    Display display;
    display.queue = {{1}, {2}};
    display.planeFencePending = true;
    display.planeFencing = {1, 0};
    display.retireThrough(2, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeFencePending = false;
    display.retireThrough(1, 110);
    EXPECT_EQ(display.retired.size(), 2u);
}

TEST(DisplayRetirement, DuplicateAndOutOfOrderNotificationsDoNotPublishAnotherFrame)
{
    Display display;
    display.queue = {{1}, {2}};
    display.retireThrough(2, 100);
    display.retireThrough(1, 110);
    display.retireThrough(2, 120);
    EXPECT_EQ(display.shown.size(), 1u);
    EXPECT_EQ(display.kicks, 1u);
}

TEST(DisplayRetirement, RecoveryKeepsOriginalTicketUntilRestoredCompositionLatches)
{
    Display display;
    display.queue = {{1}, {2}, {3}};
    display.planeRecovering = true;
    display.planeRecoveryTarget = {3, 1};
    display.retireThrough(3, 100);
    EXPECT_TRUE(display.retired.empty());
    display.planeRecovering = false;
    display.planeLatch.armed = 0;
    display.planes[0] = {3, 1};
    display.retireThrough(3, 110);
    EXPECT_TRUE(display.retired.empty());
    display.planeLatch.armed = -1;
    display.retireThrough(3, 120);
    EXPECT_EQ(display.retired, (std::vector<uint64_t>{1, 2, 3}));
}
}  // namespace
