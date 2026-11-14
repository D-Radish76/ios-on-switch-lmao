// SPDX-License-Identifier: MIT
// A render state the pack lacks, created on the raw facade over the kernel model with the shader compiler broker's answers
// set by each test: where the compiler makes no record the creation fails with an NSError as Metal's does, never handing
// back a state whose draws go elsewhere, and the client compiles nothing itself. The m2v stand-in makes each library's twin
// of the same metallib (raw_facade_compute.h), so the facade keys the state as on the device.
#include "raw_facade_compute.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/raw_compile_request.h"
#include "../raw/runtime/raw_render_key.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLLibrary> library;
};

// The facade completing the states its pack lacks, the broker answering as set, and a library of two functions
Opened opened(uint32_t flags, std::vector<int32_t> results = {TegraGM20BCompileUnavailable})
{
    kernel_model::model.compilerFlags = flags;
    kernel_model::model.compileResults = std::move(results);
    id<MTLDevice> device =
        open(writePack(packKey(MTLPixelFormatBGRA8Unorm)), [HarnessM2VMethods class], [HarnessComputeM2V class]);
    if (!device)
        return {};
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    raw_metal_complete_states(cache.c_str(), nullptr);
    const std::string bytes = made_metallib::universal({"v", "f"}, 0x61, 0xEE, {0, 1});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    return {device, [device newLibraryWithData:data error:&error]};
}

MTLRenderPipelineDescriptor *unpacked(id<MTLLibrary> library)
{
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatRGBA8Unorm);
    d.label = @"unpacked";
    return d;
}

// Its creation's error text, "" for a state made; the client compiled nothing itself
std::string created(id<MTLDevice> device, MTLRenderPipelineDescriptor *d)
{
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    EXPECT_EQ([counters(device)[@"compiled_here"] intValue], 0);
    if (state)
        return "";
    return error ? text(error.localizedDescription) : "nil with no error";
}

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;

TEST(RawFacadeCreation, NoCompilerComingFailsTheCreationWithNoAsk)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined);
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

TEST(RawFacadeCreation, ARequestTheCompilerDoesNotTakeIsAskedOnceMoreThenFails)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler did not take the state"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 2u);
    });
}

TEST(RawFacadeCreation, ARecordTheStoreDoesNotKeepIsAskedOnceMoreThenFails)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileMade});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler's store does not keep the state"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 2u);
    });
}

TEST(RawFacadeCreation, ACompileThatCouldNotRunFailsTheCreationAfterOneRetry)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileFailed});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the state's compile could not run"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 1u);
    });
}

// Another device's function names no library a request can carry: Metal refuses such a state
TEST(RawFacadeCreation, AnotherDevicesLinkedFunctionFailsTheCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileMade});
        ASSERT_TRUE(library != nil);
        MTLRenderPipelineDescriptor *d = unpacked(library);
        HarnessFunction *foreign = [HarnessFunction new];
        foreign->_name = @"f";
        foreign->_metallib = [NSData dataWithBytes:"" length:0];
        MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
        linked.functions = @[ (id<MTLFunction>)foreign ];
        d.fragmentLinkedFunctions = linked;
        const std::string why = created(device, d);
        EXPECT_FALSE(why.empty());
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

TEST(RawFacadeCreation, ACompletionHandlersCreationFailsAsTheSynchronousOneDoes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block id<MTLRenderPipelineState> state = nil;
        __block NSError *error = nil;
        [device newRenderPipelineStateWithDescriptor:unpacked(library)
                                   completionHandler:^(id<MTLRenderPipelineState> made, NSError *failed) {
                                       state = made;
                                       error = failed;
                                       dispatch_semaphore_signal(answered);
                                   }];
        ASSERT_EQ(dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
        EXPECT_EQ(state, nil);
        EXPECT_FALSE(text(error.localizedDescription).empty()) << text(error);
    });
}

// The shader compiler refusing every render state it is asked for, its refusal kept in its store as SwitchShaderCompiler
// keeps one, under the name the request's own key and libraries give
struct RefusingCompiler {
    switch_seed_pack::StoreWriter store;

    bool serve()
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0x55;
        std::string why;
        if (!store.open(dir() + "/store", identity, 64, 1u << 20, why))
            return false;
        kernel_model::model.compilerFlags = kServing | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            raw::CompileRequest r;
            if (!raw::decodeRequest(bytes, r) || r.kind != raw::RequestKind::Render || r.stages.size() < 2)
                return int32_t(TegraGM20BCompileUnavailable);
            std::map<std::string, raw::AirHash> hashes;
            const std::string &library = r.libraries[r.stages[0].library].bytes;
            if (!raw::metallibAirHashes(reinterpret_cast<const uint8_t *>(library.data()), library.size(), hashes))
                return int32_t(TegraGM20BCompileUnavailable);
            bool own = false;
            const std::string packKey = raw::requestPackKey(r, own);
            const raw::CacheKey k = raw::renderCacheKey(r.stack, packKey, hashes[r.stages[0].function],
                                                        hashes[r.stages[1].function], {}, own);
            std::vector<raw::LibraryIdentity> ids;
            for (const raw::RequestLibrary &l : r.libraries)
                ids.push_back(raw::identityOf(l));
            const std::string entry = raw::encodeEntry(k, nullptr, "a state this compiler refuses");
            const raw::Digest name = raw::storeName(k, r, ids);
            store.put(name.data(), reinterpret_cast<const uint8_t *>(entry.data()), uint32_t(entry.size()));
            return int32_t(TegraGM20BCompileRefused);
        };
        return true;
    }
};

// A state the shader compiler refused fails its creation with the refusal, as Metal's does for a state it could not
// compile, synchronously and through a completion handler; it is never m2v's
TEST(RawFacadeCreation, AStateTheCompilerRefusedFailsTheCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        RefusingCompiler compiler;
        ASSERT_TRUE(compiler.serve());
        const auto [device, library] = opened(kernel_model::model.compilerFlags);
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler refused the state: a state this compiler refuses"), std::string::npos)
            << why;
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block id<MTLRenderPipelineState> state = nil;
        __block NSError *error = nil;
        [device newRenderPipelineStateWithDescriptor:unpacked(library)
                                   completionHandler:^(id<MTLRenderPipelineState> made, NSError *failed) {
                                       state = made;
                                       error = failed;
                                       dispatch_semaphore_signal(answered);
                                   }];
        ASSERT_EQ(dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
        EXPECT_EQ(state, nil);
        EXPECT_NE(text(error.localizedDescription).find("the shader compiler refused the state"), std::string::npos)
            << text(error);
        EXPECT_EQ(gOpens, 0);
    });
}

}  // namespace
