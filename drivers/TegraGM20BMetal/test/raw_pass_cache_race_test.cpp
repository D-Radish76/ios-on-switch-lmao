// SPDX-License-Identifier: MIT
// The pass cache (RawMetalPassCache.mm) while the memory release trims it (rmPassTrim, GpuMemoryRelease's dispatch
// callback) and its thread looks up and keeps passes, thread-sanitized: the two meet only through the cache's flags, so
// a trim takes copies only where the thread is outside its window, and the thread never reads an entry a trim took.
// ios17's raw_facade_handlers_tsan_test reported rmPassTrim against rmPassFind when the trim stopped the thread with
// thread_suspend, an edge the sanitizer does not see.
#import <Metal/Metal.h>
#include "../raw/runtime/RawMetalPassCache.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kShapes = 8, kLookups = 4000;

TEST(RawPassCacheRace, ATrimAndTheThreadsLookupsMeetOnlyThroughTheCachesFlags)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        GTEST_SKIP() << "no Metal device on this host";
    std::atomic<bool> stop{false};
    std::atomic<int> hits{0}, read{0}, looked{0};
    std::thread encoder([&] {
        @autoreleasepool {
            MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:64
                                                                                        height:64
                                                                                     mipmapped:NO];
            t.storageMode = MTLStorageModePrivate;
            t.usage = MTLTextureUsageRenderTarget;
            RMRenderEncoder *e = [RMRenderEncoder new];
            std::vector<MTLRenderPassDescriptor *> passes;
            for (int i = 0; i < kShapes; i++) {
                MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
                d.colorAttachments[0].texture = [device newTextureWithDescriptor:t];
                d.colorAttachments[0].loadAction = i % 2 ? MTLLoadActionLoad : MTLLoadActionClear;
                d.colorAttachments[0].storeAction = MTLStoreActionStore;
                passes.push_back(d);
            }
            std::vector<uint8_t> key;
            uint64_t hash = 0;
            for (int n = 0; n < kLookups; n++) {
                MTLRenderPassDescriptor *d = passes[size_t(n % kShapes)];
                if (!rmPassKey(d, key, hash)) {
                    ADD_FAILURE() << "no key for Metal's own descriptor";
                    break;
                }
                if (const RMPassEntry *kept = rmPassFind(key, hash)) {
                    read += kept->pass.colorAttachments[0].texture == d.colorAttachments[0].texture;
                    hits++;
                    rmPassDone();
                } else {
                    RMPass facts{};
                    facts.color[0].texture = d.colorAttachments[0].texture;
                    rmPassKeep(key, hash, [d copy], facts, e);
                }
                looked.store(n + 1, std::memory_order_relaxed);
            }
        }
        stop.store(true);
    });
    // a trim every two rounds of the shapes, so the thread finds what it kept between them and keeps again after
    size_t trims = 0, taken = 0;
    for (int seen = 0; !stop.load();) {
        const int now = looked.load(std::memory_order_relaxed);
        if (now - seen < 2 * kShapes)
            continue;
        seen = now;
        taken += rmPassTrim().passes;
        trims++;
    }
    encoder.join();
    EXPECT_EQ(read.load(), hits.load()) << "every pass found was the one kept for it";
    EXPECT_GT(trims, 0u);
    printf("lookups %d hits %d trims %zu copies taken %zu\n", kLookups, hits.load(), trims, taken);
}

}  // namespace
