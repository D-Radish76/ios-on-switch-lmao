// SPDX-License-Identifier: MIT
// Libraries the system's Metal front end makes, through the system shader compiler and never m2v
// (raw/runtime/RawMetalCompiledLibraries.mm): Metal source, a stitched library, MPS's kernel DAG and Core Image's
// filter-function link.
// The kernel model's compiler keeps the code generation request each one sends and answers it with a metallib in the
// store; the library is then the raw device's own, its functions raw's.
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_codegen_request.h"
#include "../raw/runtime/raw_compile_request.h"
#include "../raw/runtime/raw_library_entry.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "ImageFilterLink.h"
#include "bazel/testing/fresh_process.h"

#include <dispatch/dispatch.h>
#include <gtest/gtest.h>

@protocol HarnessDagWriter
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs;
@end

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

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;

// The shader compiler as the kernel model runs it for a library: each request kept, and answered with `made` (or, with
// `log` set, the front end's error) in the store under the request's own key and name
struct LibraryCompiler {
    switch_seed_pack::StoreWriter store;
    std::vector<raw::CompileRequest> asked;
    raw::MadeLibrary made;
    std::string log;

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
            if (!raw::decodeRequest(bytes, r) || r.kind != raw::RequestKind::Library)
                return int32_t(TegraGM20BCompileUnavailable);
            asked.push_back(r);
            const raw::CacheKey k = raw::libraryCacheKey(r.stack, r.codeGenType, r.codeGen);
            if (k.key != r.key || !raw::codegen::requestOk(r.codeGenType, r.codeGen))
                return int32_t(TegraGM20BCompileUnavailable);
            const std::string entry = raw::encodeLibraryEntry(k, log.empty() ? &made : nullptr, log);
            store.put(raw::storeName(k, r, {}).data(), reinterpret_cast<const uint8_t *>(entry.data()),
                      uint32_t(entry.size()), true);
            return int32_t(log.empty() ? TegraGM20BCompileMade : TegraGM20BCompileRefused);
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

const std::string kAdd = made_metallib::wrappedBitcode(0x41), kMul = made_metallib::wrappedBitcode(0x42, 96);

// A library the device read of data, as Core Image's stitchable functions are: "add" and "mul", visible functions whose
// bitcode the metallib places
id<MTLLibrary> stitchable(id<MTLDevice> device)
{
    const std::string bytes = made_metallib::thin({"add", "mul"}, {4, 4}, {kAdd, kMul});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    return [device newLibraryWithData:data error:nil];
}

MTLStitchedLibraryDescriptor *stitching(id<MTLLibrary> library)
{
    MTLFunctionStitchingInputNode *a = [[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:0];
    MTLFunctionStitchingInputNode *b = [[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:1];
    MTLFunctionStitchingFunctionNode *add =
        [[MTLFunctionStitchingFunctionNode alloc] initWithName:@"add" arguments:@[ a, b ] controlDependencies:@[]];
    MTLFunctionStitchingFunctionNode *mul =
        [[MTLFunctionStitchingFunctionNode alloc] initWithName:@"mul" arguments:@[ add, b ] controlDependencies:@[]];
    MTLStitchedLibraryDescriptor *d = [MTLStitchedLibraryDescriptor new];
    d.functionGraphs = @[ [[MTLFunctionStitchingGraph alloc] initWithFunctionName:@"stitched"
                                                                            nodes:@[ add ]
                                                                       outputNode:mul
                                                                       attributes:@[]] ];
    d.functions = @[ [library newFunctionWithName:@"add"], [library newFunctionWithName:@"mul"] ];
    return d;
}

// A library from source is the front end's metallib, read by the raw device: its request is the source and the options
// Metal passes (no module cache: the compiler's own), asked once for every library of the same source
void sourceLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib = made_metallib::thin({"square"}, {2}, {made_metallib::wrappedBitcode(0x51)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    NSString *source = @"kernel void square(device uint *d [[buffer(0)]], uint i [[thread_position_in_grid]]) {}";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_EQ(error, nil);
    EXPECT_TRUE([library.functionNames isEqualToArray:@[ @"square" ]]) << text(library.functionNames);
    id<MTLFunction> square = [library newFunctionWithName:@"square"];
    ASSERT_TRUE(square != nil);
    EXPECT_EQ(square.functionType, MTLFunctionTypeKernel);
    ASSERT_EQ(compiler.asked.size(), 1u);
    const raw::CompileRequest &asked = compiler.asked[0];
    EXPECT_EQ(asked.codeGenType, raw::kCodeGenSource);
    std::string_view sent, options;
    ASSERT_TRUE(raw::codegen::readSource(asked.codeGen, sent, options));
    EXPECT_EQ(sent, source.UTF8String);
    EXPECT_EQ(std::string(options).rfind("--driver-mode=metal -x metal -std=metal", 0), 0u) << options;
    EXPECT_EQ(options.find("-fmodules-cache-path"), std::string_view::npos) << options;

    EXPECT_TRUE([device newLibraryWithSource:source options:nil error:&error] != nil) << text(error);
    EXPECT_EQ(compiler.asked.size(), 1u) << "the second library of the source is the store's";

    MTLCompileOptions *options2 = [MTLCompileOptions new];
    options2.preprocessorMacros = @{@"TILE" : @16, @"NAME" : @"a b"};
    options2.fastMathEnabled = NO;
    compiler.made.warnings = "program_source:1:1: warning: unused variable";
    id<MTLLibrary> warned = [device newLibraryWithSource:source options:options2 error:&error];
    EXPECT_TRUE(warned != nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorCompileWarning)) << text(error);
    ASSERT_EQ(compiler.asked.size(), 2u);
    ASSERT_TRUE(raw::codegen::readSource(compiler.asked[1].codeGen, sent, options));
    EXPECT_NE(options.find("-fno-fast-math"), std::string_view::npos) << options;
    EXPECT_NE(options.find("\"-DNAME=a b\""), std::string_view::npos) << options;
    expectNoM2V(device);
}

// Source the front end refuses is nil with Metal's compile failure and the front end's log, asked again at every
// library: a refusal never stands in the store
void refusedSource()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.log = "program_source:1:13: error: expected ')'";
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    for (unsigned asks = 1; asks <= 2; asks++) {
        NSError *error = nil;
        EXPECT_EQ([device newLibraryWithSource:@"kernel void k( {}" options:nil error:&error], nil);
        EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorCompileFailure)) << text(error);
        EXPECT_EQ(text(error.localizedDescription), compiler.log);
        EXPECT_EQ(compiler.asked.size(), asks);
    }
    NSError *error = nil;
    MTLCompileOptions *dynamic = [MTLCompileOptions new];
    dynamic.libraryType = MTLLibraryTypeDynamic;
    EXPECT_EQ([device newLibraryWithSource:@"kernel void k() {}" options:dynamic error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorUnsupported));
    EXPECT_EQ(compiler.asked.size(), 2u);
    expectNoM2V(device);
}

// With no compiler this boot a library from source or stitching fails as Metal's does without its compiler
void noCompiler()
{
    kernel_model::model.compilerFlags = TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined;
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    EXPECT_EQ([device newLibraryWithSource:@"kernel void k() {}" options:nil error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorInternal));
    EXPECT_NE(text(error.localizedDescription).find("no shader compiler comes this boot"), std::string::npos)
        << text(error);
    error = nil;
    EXPECT_EQ([device newLibraryWithStitchedDescriptor:stitching(stitchable(device)) error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorInternal)) << text(error);
    EXPECT_EQ(kernel_model::model.compiles, 0u);
    expectNoM2V(device);
}

// A stitched library's request is the base device's DAG text and each function's bitcode from the metallib the raw
// device read it of, as the builder writes them; the stitched function is raw's own. Each form Metal and Core Image
// reach it by takes the same path
void stitchedLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib = made_metallib::thin({"stitched"}, {4}, {made_metallib::wrappedBitcode(0x61)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = stitchable(device);
    ASSERT_TRUE(functions != nil);
    MTLStitchedLibraryDescriptor *d = stitching(functions);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithStitchedDescriptor:d error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_TRUE([library.functionNames isEqualToArray:@[ @"stitched" ]]) << text(library.functionNames);
    id<MTLFunction> stitched = [library newFunctionWithName:@"stitched"];
    ASSERT_TRUE(stitched != nil);
    EXPECT_EQ(stitched.functionType, MTLFunctionTypeVisible);

    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0].codeGenType, raw::kCodeGenStitch);
    NSString *dag = [(id<HarnessDagWriter>)device newDagStringWithGraphs:d.functionGraphs];
    const raw::codegen::Bitcode add = {reinterpret_cast<const uint8_t *>(kAdd.data()), kAdd.size()},
                                mul = {reinterpret_cast<const uint8_t *>(kMul.data()), kMul.size()};
    EXPECT_EQ(compiler.asked[0].codeGen, raw::codegen::stitchRequest(dag.UTF8String, {add, mul}));

    for (const char *form : {"newLibraryWithDescriptorSPI:error:", "newLibraryWithStitchedDescriptorSPI:error:"})
        EXPECT_TRUE(((id (*)(id, SEL, id, NSError **))objc_msgSend)(device, sel_registerName(form), d, &error) != nil)
            << form << ": " << text(error);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block id<MTLLibrary> later = nil;
    [device newLibraryWithStitchedDescriptor:d
                           completionHandler:^(id<MTLLibrary> made, NSError *) {
                               later = made;
                               dispatch_semaphore_signal(done);
                           }];
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)), 0);
    EXPECT_TRUE(later != nil);
    EXPECT_EQ(compiler.asked.size(), 1u) << "every form after the first is the store's";

    MTLStitchedLibraryDescriptor *foreign = [d copy];
    const std::string placeless = made_metallib::universal({"add", "mul"}, 0xA0, 0xEE, {4, 4});
    dispatch_data_t data =
        dispatch_data_create(placeless.data(), placeless.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> unplaced = [device newLibraryWithData:data error:nil];
    foreign.functions = @[ [unplaced newFunctionWithName:@"add"], [unplaced newFunctionWithName:@"mul"] ];
    EXPECT_EQ([device newLibraryWithStitchedDescriptor:foreign error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorFunctionNotFound)) << text(error);
    EXPECT_EQ(compiler.asked.size(), 1u);
    expectNoM2V(device);
}

// MPSKernelDAG's library (MPSGraph's control flow and fused kernels, which abort on nil): its DAG text as MPS writes it
// (one graph per function it makes, ALWAYS_INLINE, nodes over INPUTs and earlier NODEs) and the visible functions its
// nodes name, in MPS's order, which is not the DAG's. The request is that text and those functions' bitcode in that
// order; the library's functions are the DAG's, raw's own
void kernelDagLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib =
        made_metallib::thin({"prefixPrimary_i8", "postfixPrimary_i8"}, {4, 4},
                            {made_metallib::wrappedBitcode(0x81), made_metallib::wrappedBitcode(0x82)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = stitchable(device);
    ASSERT_TRUE(functions != nil);
    NSString *dag = @"DAGS {\n"
                    @"prefixPrimary_i8 {\n"
                    @"ALWAYS_INLINE;\n"
                    @"NODE(Node_0) : FUNCTION(add), INPUT(0), INPUT(1);\n"
                    @"OUTPUT(Node_value) : FUNCTION(mul), NODE(Node_0), INPUT(1);\n"
                    @"}\n"
                    @"postfixPrimary_i8 {\n"
                    @"ALWAYS_INLINE;\n"
                    @"NODE(Node_0) : FUNCTION(mul), INPUT(0), INPUT(1), INPUT(2);\n"
                    @"NODE(add) : FUNCTION(add), INPUT(0), NODE(Node_0);\n"
                    @"}\n"
                    @"}\n";
    NSArray<id<MTLFunction>> *named = @[ [functions newFunctionWithName:@"mul"], [functions newFunctionWithName:@"add"] ];
    NSError *error = nil;
    id<MTLLibrary> (*withDag)(id, SEL, NSString *, NSArray *, NSError **) =
        (id<MTLLibrary> (*)(id, SEL, NSString *, NSArray *, NSError **))objc_msgSend;
    const SEL selector = sel_registerName("newLibraryWithDAG:functions:error:");
    id<MTLLibrary> library = withDag(device, selector, dag, named, &error);
    ASSERT_TRUE(library != nil) << text(error);
    NSArray<NSString *> *names = [library.functionNames sortedArrayUsingSelector:@selector(compare:)];
    NSArray<NSString *> *dagFunctions = @[ @"postfixPrimary_i8", @"prefixPrimary_i8" ];
    EXPECT_TRUE([names isEqualToArray:dagFunctions]) << text(names);
    id<MTLFunction> prefix = [library newFunctionWithName:@"prefixPrimary_i8"];
    ASSERT_TRUE(prefix != nil);
    EXPECT_EQ(prefix.functionType, MTLFunctionTypeVisible);
    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0].codeGenType, raw::kCodeGenStitch);
    const raw::codegen::Bitcode add = {reinterpret_cast<const uint8_t *>(kAdd.data()), kAdd.size()},
                                mul = {reinterpret_cast<const uint8_t *>(kMul.data()), kMul.size()};
    EXPECT_EQ(compiler.asked[0].codeGen, raw::codegen::stitchRequest(dag.UTF8String, {mul, add}));

    EXPECT_EQ(withDag(device, selector, @"", named, &error), nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorCompileFailure)) << text(error);
    EXPECT_EQ(withDag(device, selector, dag, @[], &error), nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorCompileFailure)) << text(error);
    EXPECT_EQ(compiler.asked.size(), 1u);
    expectNoM2V(device);
}

// Core Image's filter-function link is ImageFilterLink.h's request of the functions' bitcode and infos
void filterLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib = made_metallib::thin({"ciKernelMain"}, {2}, {made_metallib::wrappedBitcode(0x71)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = stitchable(device);
    const switch_metal::ImageFilterArgument argument = {3, {1, 2}};
    const switch_metal::ImageFilterFunctionInfo infos[2] = {{true, 1, &argument}, {false, 0, nullptr}};
    NSError *error = nil;
    id<MTLLibrary> library = ((id (*)(id, SEL, NSArray *, const void *, NSError **))objc_msgSend)(
        device, sel_registerName("newLibraryWithImageFilterFunctionsSPI:imageFilterFunctionInfo:error:"),
        @[ [functions newFunctionWithName:@"add"], [functions newFunctionWithName:@"mul"] ], infos, &error);
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_TRUE([library.functionNames isEqualToArray:@[ @"ciKernelMain" ]]) << text(library.functionNames);
    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0].codeGenType, raw::kCodeGenImageFilters);
    std::vector<switch_metal::ImageFilterFunction> linked(2);
    linked[0] = {"add", reinterpret_cast<const uint8_t *>(kAdd.data()), kAdd.size(), true, {argument}};
    linked[1] = {"mul", reinterpret_cast<const uint8_t *>(kMul.data()), kMul.size(), false, {}};
    const std::vector<uint8_t> request = switch_metal::imageFilterLinkRequest(linked);
    EXPECT_EQ(compiler.asked[0].codeGen, std::string(request.begin(), request.end()));
    expectNoM2V(device);
}

}  // namespace

TEST(RawFacadeCompiledLibraries, ALibraryFromSourceIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(sourceLibrary);
}

TEST(RawFacadeCompiledLibraries, SourceTheFrontEndRefusesFailsAsMetalsCompileFailure)
{
    switch_ios::test::ExpectPassesInFreshProcess(refusedSource);
}

TEST(RawFacadeCompiledLibraries, WithNoCompilerALibraryFailsItsCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(noCompiler);
}

TEST(RawFacadeCompiledLibraries, AStitchedLibraryIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(stitchedLibrary);
}

TEST(RawFacadeCompiledLibraries, AnMpsKernelDagLibraryIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelDagLibrary);
}

TEST(RawFacadeCompiledLibraries, ACoreImageFilterLibraryIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(filterLibrary);
}
