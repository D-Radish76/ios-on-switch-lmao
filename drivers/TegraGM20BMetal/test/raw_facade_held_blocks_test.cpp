// SPDX-License-Identifier: MIT
// What a committed, not yet completed command buffer keeps allocated through the raw facade over the kernel model, shaped
// as hw_test_app's gpu.path.encode_cost records it (encode_cost_shape.h): the blocks held over 24 command buffers
// committed one after another and held, per command buffer, each arm after a batch of its shape completed. Each held
// block is named by its allocation site and the code that made it (held_sites.h), with the timed GPU running the jobs as
// they come and held (nothing completes). The runtime's blocks a command buffer holds may not grow with its draws and
// stay within kRuntimeHeldMost; its allocations over a batch may not grow with the draws.
#include "encode_cost_shape.h"
#include "held_sites.h"
#include "timed_gpu.h"

#include <gtest/gtest.h>
#include <cmath>

namespace held_blocks_test {

using encode_cost::kBuffers;

constexpr int kDraws[2] = {8, 16};
constexpr size_t kSiteFrames = 8;
constexpr double kRuntimeHeldMost = 2, kHeldMost = 4;

struct Arm {
    double misses = 0;
    held_sites::Window window;
    std::map<std::string, unsigned> paths;
};

double profileCount(NSDictionary<NSString *, NSNumber *> *before, NSDictionary<NSString *, NSNumber *> *after,
                    NSString *key)
{
    return after[key].doubleValue - before[key].doubleValue;
}

// 24 at `draws` held after 24 of them completed and were destroyed (whose recordings the held ones take)
Arm heldArm(timed_gpu::Gpu &gpu, int draws, bool gpuHeld)
{
    Arm arm;
    gpu.hold(true);
    EXPECT_TRUE(encode_cost::settle(draws, kBuffers, [&] {
        gpu.hold(false);
    }));
    @autoreleasepool {
        NSMutableArray<id<MTLCommandBuffer>> *held = [[NSMutableArray alloc] initWithCapacity:kBuffers];
        if (gpuHeld)
            gpu.hold(true);
        NSDictionary<NSString *, NSNumber *> *profiled = encode_cost::profile();
        held_sites_start();
        for (int i = 0; i < kBuffers; i++)
            [held addObject:encode_cost::commitOne(draws)];
        size_t events = 0;
        bool overflow = false;
        const held_sites_event *log = held_sites_stop(&events, &overflow);
        arm.misses = profileCount(profiled, encode_cost::profile(), @"recording_misses") / kBuffers;
        arm.window = held_sites::collect(log, events, overflow, kSiteFrames);
        if (gpuHeld)
            gpu.hold(false);
        arm.paths = encode_cost::finish(held);
    }
    return arm;
}

// gpu.path.encode_cost's own sequence: every count's batch once, then per count a batch and the held 24, as its Swift
// task records them (no autorelease pool of their own but the held command buffers'). Per count, per held command buffer:
// the blocks held and the allocations made on every thread, the kernel model's (the kernel's on a device) left out, and
// the command buffers that found no kept recording
struct Sequence {
    double held[2] = {}, allocations[2] = {}, misses[2] = {};
};

Sequence hwTestSequence()
{
    Sequence out;
    @autoreleasepool {
        for (int draws : kDraws)
            encode_cost::finish(encode_cost::batchUnpooled(draws));
        for (int a = 0; a < 2; a++) {
            encode_cost::finish(encode_cost::batchUnpooled(kDraws[a]));
            NSDictionary<NSString *, NSNumber *> *before = encode_cost::profile();
            NSMutableArray<id<MTLCommandBuffer>> *held = [[NSMutableArray alloc] initWithCapacity:kBuffers];
            held_sites_start();
            for (int i = 0; i < kBuffers; i++)
                [held addObject:encode_cost::commitOne(kDraws[a])];
            size_t events = 0;
            bool overflow = false;
            const held_sites_event *log = held_sites_stop(&events, &overflow);
            out.misses[a] = profileCount(before, encode_cost::profile(), @"recording_misses") / kBuffers;
            const held_sites::Window w = held_sites::collect(log, events, overflow, kSiteFrames);
            const held_sites::Window all = held_sites::collect(log, events, overflow, kSiteFrames, true);
            out.held[a] = double(w.live - w.earlierFreed - w.held(held_sites::Owner::Model)) / kBuffers;
            out.allocations[a] = double(all.held(held_sites::Owner::Runtime)) / kBuffers;
            encode_cost::finish(held);
        }
    }
    return out;
}

}  // namespace held_blocks_test

using namespace held_blocks_test;

TEST(RawFacadeHeldBlocks, ACommittedCommandBufferHoldsNoBlockOfTheRuntimePerDraw)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    ASSERT_NE(encode_cost::profile()[@"recording_misses"], nil) << "no recording counts in the encode profile";
    timed_gpu::Gpu gpu;
    for (int draws : kDraws)
        EXPECT_TRUE(encode_cost::settle(draws));
    for (const bool gpuHeld : {false, true}) {
        double runtime[2] = {};
        for (int a = 0; a < 2; a++) {
            const Arm arm = heldArm(gpu, kDraws[a], gpuHeld);
            const held_sites::Window &w = arm.window;
            EXPECT_LE(arm.misses, 0.1) << kDraws[a] << " draws, gpu " << (gpuHeld ? "held" : "running");
            for (const auto &[path, n] : arm.paths)
                EXPECT_EQ(path, "raw") << n << " command buffers at " << kDraws[a] << " draws";
            EXPECT_FALSE(w.overflow);
            runtime[a] = double(w.held(held_sites::Owner::Runtime)) / kBuffers;
            EXPECT_LE(runtime[a], kRuntimeHeldMost) << "the runtime's blocks a command buffer holds at " << kDraws[a]
                                                    << " draws, gpu " << (gpuHeld ? "held" : "running");
        }
        EXPECT_LT(runtime[1] - runtime[0], 0.5) << "the runtime's held blocks grow with the draws: " << runtime[0]
                                                << " at " << kDraws[0] << ", " << runtime[1] << " at " << kDraws[1];
    }
}

TEST(RawFacadeHeldBlocks, TheHwTestsSequenceHoldsAndAllocatesNoMoreWithItsDraws)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    ASSERT_NE(encode_cost::profile()[@"recording_misses"], nil) << "no recording counts in the encode profile";
    timed_gpu::Gpu gpu;
    const Sequence s = hwTestSequence();
    for (int a = 0; a < 2; a++) {
        EXPECT_LE(s.held[a], kHeldMost) << kDraws[a] << " draws";
        EXPECT_LE(s.misses[a], 0.1) << kDraws[a] << " draws";
    }
    EXPECT_LT(s.held[1] - s.held[0], 0.5);
    EXPECT_LT(s.allocations[1] - s.allocations[0], 0.5);
}

// The runtime's allocations on every thread over a batch, by site: the device's encode profile counts the encoding
// thread's, which here include the kernel model's records of each job (the kernel's own memory on a device)
TEST(RawFacadeHeldBlocks, ACommandBuffersAllocationsDoNotGrowWithItsDraws)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    timed_gpu::Gpu gpu;
    // A zero-cost GPU's idle drains follow CPU encoding time; both arms use the same GPU work per job.
    gpu.setPerJob(std::chrono::milliseconds(1));
    const auto settle = [&](int draws) {
        gpu.hold(true);
        return encode_cost::settle(draws, kBuffers, [&] {
            gpu.hold(false);
        });
    };
    double runtime[2] = {};
    for (int a = 0; a < 2; a++) {
        EXPECT_TRUE(settle(kDraws[a]));
        EXPECT_TRUE(settle(kDraws[a]));
        held_sites_start();
        EXPECT_TRUE(settle(kDraws[a]));
        size_t events = 0;
        bool overflow = false;
        const held_sites_event *log = held_sites_stop(&events, &overflow);
        const held_sites::Window w = held_sites::collect(log, events, overflow, kSiteFrames, true);
        runtime[a] = double(w.held(held_sites::Owner::Runtime)) / kBuffers;
    }
    EXPECT_LT(runtime[1] - runtime[0], 0.5) << "the runtime's allocations grow with the draws: " << runtime[0] << " at "
                                            << kDraws[0] << ", " << runtime[1] << " at " << kDraws[1];
}
