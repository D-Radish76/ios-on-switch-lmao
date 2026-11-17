// SPDX-License-Identifier: MIT
// The pass cache's key (RawMetalPassCache.mm) over Metal's own descriptor objects: passes that differ only in their
// depth attachment's resolve filter are two keys and two kept entries, so a kept pass never hands a later one its depth
// resolve facts (RMPass::depthFilter); a compositor frame's passes (boot 21: 29-40 render targets a command buffer) are
// all kept from one frame to the next. The attachments are the facade's textures with no device behind them: the key
// reads only their identities, and a returned recording's copies keep none of them alive.
#import <Metal/Metal.h>
#include "../raw/runtime/RawMetalPassCache.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace {

MTLRenderPassDescriptor *resolvingPass(RMTexture *depth, RMTexture *resolve, MTLMultisampleDepthResolveFilter filter)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.depthAttachment.texture = (id<MTLTexture>)depth;
    d.depthAttachment.loadAction = MTLLoadActionClear;
    d.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;
    d.depthAttachment.resolveTexture = (id<MTLTexture>)resolve;
    d.depthAttachment.depthResolveFilter = filter;
    return d;
}

MTLRenderPassDescriptor *colourPass(RMTexture *target, MTLLoadAction load = MTLLoadActionClear)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = (id<MTLTexture>)target;
    d.colorAttachments[0].loadAction = load;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    return d;
}

struct Key {
    std::vector<uint8_t> bytes;
    uint64_t hash = 0;
};

RMCommandBuffer *takenCommandBuffer()
{
    RMCommandBuffer *cb = [RMCommandBuffer new];
    rmTakeRecording(cb);
    return cb;
}

// A copy of `d` kept in `cb`'s bank, as a pass opened from it keeps one; the copy itself
MTLRenderPassDescriptor *keepPass(RMCommandBuffer *cb, MTLRenderPassDescriptor *d)
{
    Key k;
    EXPECT_TRUE(rmPassKey(d, k.bytes, k.hash));
    RMPass facts{};
    facts.color[0].texture = d.colorAttachments[0].texture;
    MTLRenderPassDescriptor *copy = [d copy];
    rmPassKeep(cb, k.bytes, k.hash, copy, facts, [RMRenderEncoder new]);
    return copy;
}

const RMPassEntry *findPass(RMCommandBuffer *cb, MTLRenderPassDescriptor *d)
{
    Key k;
    return rmPassKey(d, k.bytes, k.hash) ? rmPassFind(cb, k.bytes, k.hash) : nullptr;
}

class RawPassCache : public testing::Test {
protected:
    RMTexture *depth, *resolve;
    RMCommandBuffer *cb;

    static void SetUpTestSuite()
    {
        rmPoolCommandBuffers();
    }

    void SetUp() override
    {
        rmPassTrim();
        cb = takenCommandBuffer();
        depth = [RMTexture new];
        resolve = [RMTexture new];
    }

    void TearDown() override
    {
        if (cb)
            rmRecordingDone(cb);
    }

    Key keyOf(MTLMultisampleDepthResolveFilter filter)
    {
        Key k;
        EXPECT_TRUE(rmPassKey(resolvingPass(depth, resolve, filter), k.bytes, k.hash));
        return k;
    }
};

TEST_F(RawPassCache, TheSamePassIsTheSameKey)
{
    const Key a = keyOf(MTLMultisampleDepthResolveFilterMin), b = keyOf(MTLMultisampleDepthResolveFilterMin);
    EXPECT_EQ(a.bytes, b.bytes);
    EXPECT_EQ(a.hash, b.hash);
}

TEST_F(RawPassCache, ADepthResolveFilterIsPartOfTheKey)
{
    const Key lowest = keyOf(MTLMultisampleDepthResolveFilterMin), highest = keyOf(MTLMultisampleDepthResolveFilterMax);
    ASSERT_FALSE(lowest.bytes.empty());
    EXPECT_NE(lowest.bytes, highest.bytes);
}

TEST_F(RawPassCache, PassesOfTwoDepthResolveFiltersAreTwoEntries)
{
    const MTLMultisampleDepthResolveFilter filters[2] = {MTLMultisampleDepthResolveFilterMin,
                                                         MTLMultisampleDepthResolveFilterMax};
    Key keys[2];
    RMRenderEncoder *e = [RMRenderEncoder new];
    for (int i = 0; i < 2; i++) {
        keys[i] = keyOf(filters[i]);
        RMPass facts{};
        facts.zeta[0].texture = depth;
        facts.depthResolve = resolve;
        facts.depthFilter = uint32_t(filters[i]);
        rmPassKeep(cb, keys[i].bytes, keys[i].hash, [resolvingPass(depth, resolve, filters[i]) copy], facts, e);
    }
    const RMPassEntry *kept[2] = {};
    for (int i = 0; i < 2; i++) {
        kept[i] = rmPassFind(cb, keys[i].bytes, keys[i].hash);
        ASSERT_NE(kept[i], nullptr);
        EXPECT_EQ(kept[i]->facts.depthFilter, uint32_t(filters[i])) << i;
    }
    EXPECT_NE(kept[0], kept[1]);
}

TEST_F(RawPassCache, AFrameOfFortyPassesIsKeptForTheNext)
{
    std::vector<MTLRenderPassDescriptor *> passes;
    for (int i = 0; i < 20; i++) {
        RMTexture *target = [RMTexture new];
        for (MTLLoadAction load : {MTLLoadActionClear, MTLLoadActionLoad})
            passes.push_back(colourPass(target, load));
    }
    for (MTLRenderPassDescriptor *d : passes)
        if (!findPass(cb, d))
            keepPass(cb, d);
    rmRecordingDone(cb);
    cb = takenCommandBuffer();
    size_t found = 0;
    for (MTLRenderPassDescriptor *d : passes)
        found += findPass(cb, d) != nullptr;
    EXPECT_EQ(found, passes.size());
}

TEST_F(RawPassCache, ACommandBufferLeavesNoTextureItUsedAliveOnceItReturns)
{
    __weak RMTexture *gone;
    @autoreleasepool {
        RMTexture *target = [RMTexture new];
        gone = target;
        RMCommandBuffer *b = takenCommandBuffer();
        keepPass(b, colourPass(target));
        rmRecordingDone(b);
    }
    EXPECT_EQ(gone, nil) << "the returned recording's kept pass still names its texture";
    EXPECT_GE(rmPassHeld().passes, 1u) << "the pass itself is kept";
    EXPECT_EQ(rmPassHeld().textures, 0u);
}

TEST_F(RawPassCache, ABurstOfTwentyFourLeavesNoTextureAliveOnceItReturns)
{
    constexpr int kBurst = 24;
    __weak RMTexture *gone[kBurst];
    @autoreleasepool {
        std::vector<RMCommandBuffer *> burst;
        for (int i = 0; i < kBurst; i++) {
            RMTexture *target = [RMTexture new];
            gone[i] = target;
            burst.push_back(takenCommandBuffer());
            keepPass(burst.back(), colourPass(target));
        }
        for (RMCommandBuffer *b : burst)
            rmRecordingDone(b);
    }
    for (int i = 0; i < kBurst; i++)
        EXPECT_EQ(gone[i], nil) << "texture " << i << " outlived its command buffer";
}

TEST_F(RawPassCache, ACommandBufferOnTheRawPathNeverNamesTexturesInAFoundCopy)
{
    RMTexture *target = [RMTexture new];
    MTLRenderPassDescriptor *d = colourPass(target);
    RMCommandBuffer *first = takenCommandBuffer();
    MTLRenderPassDescriptor *copy = keepPass(first, d);
    rmRecordingDone(first);
    EXPECT_EQ(copy.colorAttachments[0].texture, nil) << "the returned copy still names the texture";
    RMCommandBuffer *next = takenCommandBuffer();
    const RMPassEntry *kept = findPass(next, d);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->pass, copy) << "the pass was copied again";
    EXPECT_EQ(copy.colorAttachments[0].texture, nil) << "a hit named the texture with no replay asking";
    rmRecordingDone(next);
    EXPECT_EQ(copy.colorAttachments[0].texture, nil);
}

TEST_F(RawPassCache, AReplayFindsTheTexturesInTheCopyUntilItsCommandBufferReturns)
{
    __weak RMTexture *gone;
    MTLRenderPassDescriptor *copy = nil;
    @autoreleasepool {
        RMTexture *target = [RMTexture new];
        gone = target;
        MTLRenderPassDescriptor *d = colourPass(target);
        RMCommandBuffer *first = takenCommandBuffer();
        keepPass(first, d);
        rmRecordingDone(first);
        RMCommandBuffer *next = takenCommandBuffer();
        const RMPassEntry *kept = findPass(next, d);
        ASSERT_NE(kept, nullptr);
        copy = kept->pass;
        rmPassBind(next, copy);
        EXPECT_EQ(copy.colorAttachments[0].texture, (id<MTLTexture>)target) << "the replay's copy names no texture";
        rmRecordingDone(next);
        EXPECT_EQ(copy.colorAttachments[0].texture, nil) << "the replayed copy still names the texture";
    }
    EXPECT_EQ(gone, nil);
}

TEST_F(RawPassCache, ANewTextureWhereADeadOneWasFindsNothingOfIt)
{
    RMTexture *target = [RMTexture new];
    MTLRenderPassDescriptor *d = colourPass(target);
    RMCommandBuffer *first = takenCommandBuffer();
    keepPass(first, d);
    rmRecordingDone(first);
    RMCommandBuffer *next = takenCommandBuffer();
    ASSERT_NE(findPass(next, d), nullptr);
    target->_identity |= uint64_t(1) << 63;  // another texture at the same address
    EXPECT_EQ(findPass(next, d), nullptr) << "a kept pass of the dead texture's was found by its address";
    rmRecordingDone(next);
}

TEST_F(RawPassCache, ABatchOutAtOnceFindsItsPassesInTheNextBatch)
{
    constexpr int kBatch = 24;
    MTLRenderPassDescriptor *frame = colourPass([RMTexture new]);
    int found = 0;
    for (int round = 0; round < 2; round++) {
        std::vector<RMCommandBuffer *> batch;
        for (int i = 0; i < kBatch; i++) {
            batch.push_back(takenCommandBuffer());
            if (findPass(batch.back(), frame))
                found += round;
            else
                keepPass(batch.back(), frame);
        }
        for (RMCommandBuffer *b : batch)
            rmRecordingDone(b);
    }
    EXPECT_EQ(found, kBatch) << "a batch's kept passes were let go as they returned";
}

TEST_F(RawPassCache, ATrimLetsAReturnedRecordingsPassesGo)
{
    RMCommandBuffer *b = takenCommandBuffer();
    for (int i = 0; i < 3; i++)
        keepPass(b, colourPass([RMTexture new]));
    rmRecordingDone(b);
    EXPECT_GE(rmPassTrim().passes, 3u);
    EXPECT_EQ(rmPassHeld().passes, 0u);
}

TEST_F(RawPassCache, CountingAndTrimmingLeaveTheNextCommandBufferWhatReturnedLast)
{
    RMCommandBuffer *first = takenCommandBuffer(), *last = takenCommandBuffer();
    first->_shadow.reserve(4096);
    last->_shadow.reserve(first->_shadow.capacity() + 4096);
    const void *lastBank = last->_passCache.get();
    const size_t lastShadow = last->_shadow.capacity();
    rmRecordingDone(first);
    rmRecordingDone(last);
    rmPassHeld();
    rmPassTrim();
    RMCommandBuffer *next = takenCommandBuffer();
    EXPECT_EQ(static_cast<const void *>(next->_passCache.get()), lastBank) << "the kept recordings came back reversed";
    EXPECT_EQ(next->_shadow.capacity(), lastShadow);
    rmRecordingDone(next);
}

TEST_F(RawPassCache, ATrimLeavesAPassItsRecordingOwnsUntilItReturns)
{
    __weak RMTexture *weakTarget;
    std::promise<void> reading, trimmed, looked;
    std::promise<void> done;
    std::thread encoder([&] {
        RMCommandBuffer *b = takenCommandBuffer();
        MTLRenderPassDescriptor *d = nil;
        @autoreleasepool {
            RMTexture *target = [RMTexture new];
            weakTarget = target;
            d = colourPass(target);
            keepPass(b, d);
        }
        const RMPassEntry *kept = findPass(b, d);
        ASSERT_NE(kept, nullptr);
        d = nil;
        reading.set_value();
        trimmed.get_future().wait();
        EXPECT_NE(kept->pass, nil) << "the copy this thread reads went under it";
        rmRecordingDone(b);
        looked.set_value();
        done.get_future().wait();
    });
    reading.get_future().wait();
    auto trim = std::async(std::launch::async, [] {
        return rmPassTrim();
    });
    const bool returned = trim.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(returned) << "the trim waited for the thread inside its pass";
    EXPECT_NE(weakTarget, nil);
    trimmed.set_value();
    trim.wait();
    looked.get_future().wait();
    EXPECT_EQ(weakTarget, nil) << "the returned recording's copy still names its texture";
    done.set_value();
    encoder.join();
}

}  // namespace
