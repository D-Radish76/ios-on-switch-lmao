// SPDX-License-Identifier: MIT
// What the system shader compiler answers in place of m2v (raw/runtime/RawCompleteRemote.mm, compiler/): a compute state's
// facts the pack lacks, made once and then read from its store, and a kernel it refused, whose command buffer fails. The
// kernel model runs a stand-in compiler over a store, as SwitchShaderCompiler fills it.
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_compile_request.h"
#include "../raw/runtime/raw_compute_facts_entry.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

void expectNoM2V(id<MTLDevice> device)
{
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

NSURL *fileURL(const std::string &path)
{
    return [NSURL fileURLWithPath:@(path.c_str())];
}

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;
constexpr uint32_t kFacts[3] = {640, 32, 2048};

// The shader compiler as the kernel model runs it: each facts request answered with kFacts in the store, under the name
// the request's own state key and libraries give
struct FactsCompiler {
    switch_seed_pack::StoreWriter store;
    std::vector<std::string> asked;

    bool serve()
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0x99;
        std::string why;
        if (!store.open(dir() + "/store", identity, 64, 1u << 20, why))
            return false;
        kernel_model::model.compilerFlags = kServing | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            raw::CompileRequest r;
            constexpr char kFactsPrefix[] = "facts | ", kKernelPrefix[] = "kernel | ";
            if (!raw::decodeRequest(bytes, r))
                return int32_t(TegraGM20BCompileUnavailable);
            asked.push_back(r.key);
            std::vector<raw::LibraryIdentity> ids;
            for (const raw::RequestLibrary &l : r.libraries)
                ids.push_back(raw::identityOf(l));
            const bool facts = r.kind == raw::RequestKind::Facts && !r.key.rfind(kFactsPrefix, 0);
            const bool kernel = r.kind == raw::RequestKind::Kernel && !r.key.rfind(kKernelPrefix, 0);
            if (!facts && !kernel)
                return int32_t(TegraGM20BCompileUnavailable);
            const raw::CacheKey k = facts ? raw::factsCacheKey(r.stack, r.key.substr(sizeof kFactsPrefix - 1))
                                          : raw::kernelCacheKey(r.stack, r.key.substr(sizeof kKernelPrefix - 1));
            const std::string entry = facts ? raw::encodeFactsEntry(k, kFacts, "")
                                            : raw::encodeKernelEntry(k, nullptr, "a kernel this compiler refuses");
            const raw::Digest name = raw::storeName(k, r, ids);
            store.put(name.data(), reinterpret_cast<const uint8_t *>(entry.data()), uint32_t(entry.size()));
            return int32_t(facts ? TegraGM20BCompileMade : TegraGM20BCompileRefused);
        };
        return true;
    }
};

id<MTLDevice> completing()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    raw_metal_complete_states(cache.c_str(), nullptr);
    return device;
}

// A compute state the pack has no facts of is answered by the shader compiler at its creation, once for every state of
// the same key (the second is the store's), and its answers are the compiler's
void computeFactsFromTheCompiler()
{
    FactsCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    const std::string metallib = writeMetallib();
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(metallib) error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    id<MTLComputePipelineState> made = [device newComputePipelineStateWithFunction:kernel error:&error];
    ASSERT_TRUE(made != nil) << text(error);
    EXPECT_EQ(made.maxTotalThreadsPerThreadgroup, kFacts[0]);
    EXPECT_EQ(made.threadExecutionWidth, kFacts[1]);
    EXPECT_EQ(made.staticThreadgroupMemoryLength, kFacts[2]);
    std::map<std::string, raw::AirHash> hashes;
    ASSERT_TRUE(raw::metallibAirHashes(metallib, hashes));
    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0], "facts | " + raw::computeStateKey(hashes["k"], "", 0, false));
    id<MTLComputePipelineState> again = [device newComputePipelineStateWithFunction:kernel error:&error];
    ASSERT_TRUE(again != nil) << text(error);
    EXPECT_EQ(again.staticThreadgroupMemoryLength, kFacts[2]);
    EXPECT_EQ(compiler.asked.size(), 1u) << "the second state's facts are the store's";
    expectNoM2V(device);
}

// A dispatch whose kernel the shader compiler refused fails its command buffer, as Metal's does for a pipeline it could not
// compile, whether the refusal comes before the command buffer is recorded or at its submission; nothing goes to m2v
void refusedKernel()
{
    FactsCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:kernel error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLBuffer> out = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (int round = 0; round < 2; round++) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:state];
        [e setBuffer:out offset:0 atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusError) << "round " << round;
        EXPECT_NE(text(cb.error.localizedDescription).find("refused"), std::string::npos) << text(cb.error);
    }
    expectNoM2V(device);
}

// With no compiler this boot the creation fails as Metal's does when it cannot compile, never through m2v
void computeFactsWithNoCompiler()
{
    kernel_model::model.compilerFlags = TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined;
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    EXPECT_EQ([device newComputePipelineStateWithFunction:kernel error:&error], nil);
    EXPECT_NE(text(error.localizedDescription).find("no shader compiler comes this boot"), std::string::npos)
        << text(error);
    EXPECT_EQ(kernel_model::model.compiles, 0u);
    expectNoM2V(device);
}

}  // namespace

TEST(RawFacadeCompiler, AComputeStateThePackLacksIsAnsweredByTheShaderCompiler)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeFactsFromTheCompiler);
}

TEST(RawFacadeCompiler, AKernelTheCompilerRefusedFailsItsCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(refusedKernel);
}

TEST(RawFacadeCompiler, AComputeStateWithNoCompilerFailsItsCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeFactsWithNoCompiler);
}
