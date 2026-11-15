// SPDX-License-Identifier: MIT
#include "raw_facade_compiler_store.h"
#include "bazel/testing/fresh_process.h"
#include <gtest/gtest.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

id<MTLLibrary> functions(id<MTLDevice> device)
{
    const std::string bytes =
        made_metallib::thin({"consumer", "helper", "other_consumer"}, {2, 4, 2},
                            {made_metallib::wrappedBitcode(0xa1), made_metallib::wrappedBitcode(0xa2),
                             made_metallib::wrappedBitcode(0xa3)});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    EXPECT_TRUE(library != nil) << text(error);
    return library;
}

id<MTLFunction> kernelFunction(id<MTLLibrary> library, NSString *name)
{
    MTLFunctionDescriptor *descriptor = [MTLFunctionDescriptor functionDescriptor];
    descriptor.name = name;
    NSError *error = nil;
    id<MTLFunction> function = [library newFunctionWithDescriptor:descriptor error:&error];
    EXPECT_TRUE(function != nil) << text(error);
    return function;
}

id<MTLFunction> binaryFunction(id<MTLLibrary> library, NSString *name, uint32_t value = 0)
{
    MTLFunctionDescriptor *descriptor = [MTLFunctionDescriptor functionDescriptor];
    descriptor.name = @"helper";
    descriptor.specializedName = name;
    descriptor.options = MTLFunctionOptionCompileToBinary;
    if (value) {
        descriptor.constantValues = [MTLFunctionConstantValues new];
        [descriptor.constantValues setConstantValue:&value type:MTLDataTypeUInt atIndex:1];
    }
    NSError *error = nil;
    id<MTLFunction> function = [library newFunctionWithDescriptor:descriptor error:&error];
    EXPECT_TRUE(function != nil) << text(error);
    EXPECT_EQ(function.options, MTLFunctionOptionCompileToBinary);
    EXPECT_TRUE([function.name isEqualToString:name]);
    return function;
}

MTLComputePipelineDescriptor *descriptor(id<MTLLibrary> library, id<MTLFunction> binary)
{
    id<MTLFunction> kernel = kernelFunction(library, @"consumer");
    if (!kernel || !binary)
        return nil;
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = kernel;
    d.linkedFunctions = [MTLLinkedFunctions linkedFunctions];
    d.linkedFunctions.binaryFunctions = @[ binary ];
    return d;
}

void expectNative(id<MTLDevice> device)
{
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

void retainedBinaryDispatch()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    id<MTLComputePipelineState> state;
    @autoreleasepool {
        id<MTLLibrary> library = functions(device);
        ASSERT_TRUE(library != nil);
        MTLComputePipelineDescriptor *d = descriptor(library, binaryFunction(library, @"transform"));
        ASSERT_TRUE(d != nil);
        MTLAutoreleasedComputePipelineReflection reflection = nil;
        NSError *error = nil;
        state = [device newComputePipelineStateWithDescriptor:d
                                                      options:MTLPipelineOptionBufferTypeInfo
                                                   reflection:&reflection
                                                        error:&error];
        ASSERT_TRUE(state != nil) << text(error);
        ASSERT_TRUE(reflection != nil) << text(error);
        ASSERT_EQ(compiler.consumers.size(), 2u);
        for (const auto &request : compiler.consumers) {
            ASSERT_EQ(request.stages.size(), 2u);
            EXPECT_EQ(request.stages[1].tag, "kb");
            EXPECT_EQ(request.stages[1].function, "helper");
            EXPECT_EQ(request.stages[1].name, "transform");
        }
        id<MTLFunction> replacement = kernelFunction(library, @"other_consumer");
        ASSERT_TRUE(replacement != nil);
        d.linkedFunctions.binaryFunctions = @[];
        d.computeFunction = replacement;
    }
    const size_t jobs = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:state];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << text(commands.error);
    ASSERT_EQ(compiler.consumers.size(), 3u);
    const auto &dispatched = compiler.consumers.back();
    EXPECT_EQ(dispatched.kind, raw::RequestKind::Kernel);
    ASSERT_EQ(dispatched.stages.size(), 2u);
    EXPECT_EQ(dispatched.stages[0].function, "consumer");
    EXPECT_EQ(dispatched.stages[1].tag, "kb");
    EXPECT_EQ(dispatched.stages[1].name, "transform");
    EXPECT_TRUE(computeLaunched(jobs));
    expectNative(device);
}

void binaryIdentity()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    id<MTLLibrary> library = functions(device);
    ASSERT_TRUE(device && library);
    NSError *error = nil;
    for (const auto &[name, value] : {std::pair<NSString *, uint32_t>{@"first", 1}, {@"second", 1}, {@"first", 2}}) {
        auto *d = descriptor(library, binaryFunction(library, name, value));
        ASSERT_TRUE(d != nil);
        EXPECT_TRUE([device newComputePipelineStateWithDescriptor:d
                                                          options:MTLPipelineOptionNone
                                                       reflection:nil
                                                            error:&error] != nil)
            << text(error);
    }
    ASSERT_EQ(compiler.consumers.size(), 3u);
    EXPECT_NE(compiler.consumers[0].key, compiler.consumers[1].key);
    EXPECT_NE(compiler.consumers[0].key, compiler.consumers[2].key);
    EXPECT_NE(compiler.consumers[1].key, compiler.consumers[2].key);
    expectNative(device);
}

void invalidBinaryFunctions()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    id<MTLLibrary> library = functions(device);
    ASSERT_TRUE(device && library);
    NSError *error = nil;
    auto *d = descriptor(library, [library newFunctionWithName:@"helper"]);
    ASSERT_TRUE(d != nil);
    EXPECT_EQ(
        [device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error],
        nil);
    EXPECT_TRUE(error != nil);
    MTLFunctionDescriptor *function = [MTLFunctionDescriptor functionDescriptor];
    function.name = @"consumer";
    function.options = MTLFunctionOptionCompileToBinary;
    error = nil;
    EXPECT_EQ([library newFunctionWithDescriptor:function error:&error], nil);
    EXPECT_TRUE(error != nil);
    EXPECT_TRUE(compiler.consumers.empty());
    expectNative(device);
}

}  // namespace

TEST(RawFacadeBinaryFunctions, OwnsItsFunctionsAfterTheCallerReleasesTheDescriptorAndLibrary)
{
    switch_ios::test::ExpectPassesInFreshProcess(retainedBinaryDispatch);
}

TEST(RawFacadeBinaryFunctions, SpecializedNamesAndConstantsHaveDistinctNativeKeys)
{
    switch_ios::test::ExpectPassesInFreshProcess(binaryIdentity);
}

TEST(RawFacadeBinaryFunctions, BinaryListsRequireVisibleFunctionsMadeWithCompileToBinary)
{
    switch_ios::test::ExpectPassesInFreshProcess(invalidBinaryFunctions);
}
