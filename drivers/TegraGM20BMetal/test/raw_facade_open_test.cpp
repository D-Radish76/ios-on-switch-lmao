// SPDX-License-Identifier: MIT
// What no process opens its m2v device for: a library's own answers, a library Metal would not load, memory the kernel
// has none of, pages it cannot wire, and a YCbCr target state raw draws. Each answer is the raw device's or Metal's nil.
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_compile_request.h"
#include "../raw/runtime/raw_compute_facts_entry.h"
#include "../raw/runtime/raw_ycbcr_store.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <sys/mman.h>

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

std::string writeFile(const std::string &name, const std::string &bytes)
{
    const std::string path = dir() + "/" + name;
    std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    return path;
}

NSURL *fileURL(const std::string &path)
{
    return [NSURL fileURLWithPath:@(path.c_str())];
}

NSArray<NSString *> *externNames(id<MTLLibrary> library)
{
    return ((NSArray<NSString *> * (*)(id, SEL)) objc_msgSend)(library, sel_registerName("externFunctionNames"));
}

void libraryAnswers()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> kernels = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    ASSERT_TRUE(kernels != nil) << text(error);
    EXPECT_TRUE([kernels.functionNames isEqualToArray:(@[ @"v", @"f", @"k" ])]) << text(kernels.functionNames);
    EXPECT_EQ(externNames(kernels).count, 0u);
    EXPECT_EQ(kernels.type, MTLLibraryTypeExecutable);
    EXPECT_EQ([kernels newFunctionWithName:@"k"].options, MTLFunctionOptionNone);

    const std::string ci = made_metallib::universal({"tint", "swap_rb"}, 0xB0, 0xEE, {5, 5});
    id<MTLLibrary> externs = [device newLibraryWithFile:@(writeFile("ci.metallib", ci).c_str()) error:&error];
    ASSERT_TRUE(externs != nil) << text(error);
    EXPECT_EQ(externs.functionNames.count, 0u);
    EXPECT_TRUE([externNames(externs) isEqualToArray:(@[ @"tint", @"swap_rb" ])]) << text(externNames(externs));

    const std::string none = made_metallib::universal({});
    id<MTLLibrary> empty = [device newLibraryWithURL:fileURL(writeFile("none.metallib", none)) error:&error];
    ASSERT_TRUE(empty != nil) << "a metallib that lists no function loads: " << text(error);
    EXPECT_EQ(empty.functionNames.count, 0u);
    EXPECT_EQ([empty newFunctionWithName:@"v"], nil);
    expectNoM2V(device);
}

void unreadLibraries()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    EXPECT_EQ([device newLibraryWithURL:fileURL(dir() + "/absent.metallib") error:&error], nil);
    EXPECT_NE(error, nil);
    error = nil;
    EXPECT_EQ([device newLibraryWithURL:[NSURL URLWithString:@"https://example.invalid/default.metallib"] error:&error],
              nil);
    EXPECT_NE(error, nil);
    error = nil;
    EXPECT_EQ([device newLibraryWithFile:@(writeFile("text.metallib", "not a metallib").c_str()) error:&error], nil);
    EXPECT_EQ(error.code, MTLLibraryErrorUnsupported) << text(error);
    std::string noAir = made_metallib::universal({"f"});
    noAir[28] = 0x02;
    error = nil;
    dispatch_data_t data = dispatch_data_create(noAir.data(), noAir.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    EXPECT_EQ([device newLibraryWithData:data error:&error], nil);
    EXPECT_EQ(error.code, MTLLibraryErrorUnsupported) << text(error);
    EXPECT_EQ([device newDefaultLibrary], nil) << "the test binary's bundle has no default.metallib";
    error = nil;
    EXPECT_EQ([device newDefaultLibraryWithBundle:[NSBundle mainBundle] error:&error], nil);
    EXPECT_NE(error, nil);
    expectNoM2V(device);
}

void noMemory()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    constexpr NSUInteger kLarge = 64u << 20;
    // the pool's chunks are the task's pages imported (raw_gpu.cpp newChunk), block-linear chunks the kernel's own
    kernel_model::model.refuseAllocs = kernel_model::model.refuseImports = 1000;
    EXPECT_EQ([device newBufferWithLength:kLarge options:MTLResourceStorageModeShared], nil);
    EXPECT_EQ([device newBufferWithLength:kLarge options:MTLResourceStorageModePrivate], nil);
    std::vector<uint8_t> bytes(kLarge, 0x5A);
    EXPECT_EQ([device newBufferWithBytes:bytes.data() length:kLarge options:MTLResourceStorageModeShared], nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:4096
                                                                                height:4096
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    EXPECT_EQ([device newTextureWithDescriptor:d], nil);
    d.storageMode = MTLStorageModeShared;
    EXPECT_EQ([device newTextureWithDescriptor:d], nil);
    kernel_model::model.refuseAllocs = kernel_model::model.refuseImports = 0;
    EXPECT_NE([device newBufferWithLength:kLarge options:MTLResourceStorageModeShared], nil) << "memory again: made";
    expectNoM2V(device);
}

void unwiredPages()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const size_t length = 4 * getpagesize();
    void *pages = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    ASSERT_NE(pages, MAP_FAILED);
    kernel_model::model.refuseImports = 1;
    EXPECT_EQ(
        [device newBufferWithBytesNoCopy:pages length:length options:MTLResourceStorageModeShared deallocator:nil],
        nil);
    EXPECT_NE(
        [device newBufferWithBytesNoCopy:pages length:length options:MTLResourceStorageModeShared deallocator:nil], nil)
        << "the next import is taken";
    expectNoM2V(device);
}

// A two-plane YCbCr target's state the pack holds for its stand-in is drawn by raw from its creation: m2v's twin, and its
// functions', wait for a command buffer that falls back
void ycbcrTargetState()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormat(raw::kYcbcrTargetStandIn))));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormat(500)) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    EXPECT_TRUE([ask(device, "switchRawM2V") isKindOfClass:[NSDictionary class]]);
    EXPECT_TRUE([((NSNumber * (*)(id, SEL)) objc_msgSend)(state, sel_registerName("switchPackHit")) boolValue])
        << "drawn from the pack";
    expectNoM2V(device);
}

// The plain layout of a texture whose memory no block-linear image is made in (an uncompressed one), as m2v's frontend
// answered it; a heap's or buffer's texture has none; a no-copy texture of it is refused, as it was
void plainLayouts()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const auto layoutOf = [&](MTLTextureDescriptor *d, BOOL backed) {
        return ((id (*)(id, SEL, id, BOOL))objc_msgSend)(
            device, sel_registerName("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:"), d, backed);
    };
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:64
                                                                                height:32
                                                                             mipmapped:NO];
    id layout = layoutOf(d, NO);
    ASSERT_TRUE(layout != nil);
    EXPECT_EQ(((NSUInteger (*)(id, SEL))objc_msgSend)(layout, sel_registerName("size")), 64u * 32 * 4);
    EXPECT_EQ(layoutOf(d, YES), nil);
    std::vector<uint32_t> linear(64 * 32), memory(64 * 32), back(64 * 32);
    for (size_t i = 0; i < linear.size(); i++)
        linear[i] = uint32_t(i * 2654435761u);
    const MTLRegion all = MTLRegionMake2D(0, 0, 64, 32);
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger, NSUInteger, void *, NSUInteger, NSUInteger,
               MTLRegion))objc_msgSend)(
        layout,
        sel_registerName("copyFromLinearBytes:linearOffset:linearBytesPerRow:linearBytesPerImage:toTextureMemory:"
                         "textureSlice:textureLevel:textureRegion:"),
        linear.data(), 0, 256, 0, memory.data(), 0, 0, all);
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger, MTLRegion, void *, NSUInteger, NSUInteger,
               NSUInteger))objc_msgSend)(
        layout,
        sel_registerName("copyFromTextureMemory:textureSlice:textureLevel:textureRegion:toLinearBytes:linearOffset:"
                         "linearBytesPerRow:linearBytesPerImage:"),
        memory.data(), 0, 0, all, back.data(), 0, 256, 0);
    EXPECT_EQ(memory, linear) << "rows packed";
    EXPECT_EQ(back, linear);
    MTLTextureDescriptor *volume = [MTLTextureDescriptor new];
    volume.textureType = MTLTextureType3D;
    volume.pixelFormat = MTLPixelFormatR16Float;
    volume.width = 8;
    volume.height = 4;
    volume.depth = 3;
    id volumeLayout = layoutOf(volume, NO);
    ASSERT_TRUE(volumeLayout != nil);
    EXPECT_EQ(((NSUInteger (*)(id, SEL))objc_msgSend)(volumeLayout, sel_registerName("size")), 8u * 4 * 3 * 2);
    EXPECT_EQ(((id (*)(id, SEL, void *, NSUInteger, id, id))objc_msgSend)(
                  device, sel_registerName("newTextureWithBytesNoCopy:length:descriptor:deallocator:"), memory.data(),
                  memory.size() * 4, d, nil),
              nil);
    expectNoM2V(device);
}

// A texture whose rows run past its buffer's or surface's memory, or a memoryless one over a buffer, is nil as Metal
// answers an invalid descriptor
void texturesPastTheirMemory()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:0 bytesPerRow:256], nil) << "16 KiB of rows in 4 KiB";
    d.height = 16;
    d.storageMode = MTLStorageModeMemoryless;
    EXPECT_EQ([buffer newTextureWithDescriptor:d offset:0 bytesPerRow:256], nil);
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(surface != nullptr);
    d.storageMode = MTLStorageModeShared;
    d.height = 4096;
    EXPECT_EQ([device newTextureWithDescriptor:d iosurface:surface plane:0], nil) << "rows past the allocation";
    CFRelease(surface);
    expectNoM2V(device);
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
            constexpr char kPrefix[] = "facts | ";
            if (!raw::decodeRequest(bytes, r) || r.kind != raw::RequestKind::Facts || r.key.rfind(kPrefix, 0))
                return int32_t(TegraGM20BCompileUnavailable);
            asked.push_back(r.key);
            const raw::CacheKey k = raw::factsCacheKey(r.stack, r.key.substr(sizeof kPrefix - 1));
            std::vector<raw::LibraryIdentity> ids;
            for (const raw::RequestLibrary &l : r.libraries)
                ids.push_back(raw::identityOf(l));
            const std::string entry = raw::encodeFactsEntry(k, kFacts, "");
            const raw::Digest name = raw::storeName(k, r, ids);
            store.put(name.data(), reinterpret_cast<const uint8_t *>(entry.data()), uint32_t(entry.size()));
            return int32_t(TegraGM20BCompileMade);
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

TEST(RawFacadeOpen, AnUncompressedTextureHasThePlainLayoutAndNoNoCopyTexture)
{
    switch_ios::test::ExpectPassesInFreshProcess(plainLayouts);
}

TEST(RawFacadeOpen, ATexturePastItsMemoryIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(texturesPastTheirMemory);
}

TEST(RawFacadeOpen, AComputeStateThePackLacksIsAnsweredByTheShaderCompiler)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeFactsFromTheCompiler);
}

TEST(RawFacadeOpen, AComputeStateWithNoCompilerFailsItsCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeFactsWithNoCompiler);
}

TEST(RawFacadeOpen, ALibraryAnswersItsNamesTypeAndOptions)
{
    switch_ios::test::ExpectPassesInFreshProcess(libraryAnswers);
}

TEST(RawFacadeOpen, ALibraryMetalWouldNotLoadIsNilWithMetalsError)
{
    switch_ios::test::ExpectPassesInFreshProcess(unreadLibraries);
}

TEST(RawFacadeOpen, NoMemoryIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(noMemory);
}

TEST(RawFacadeOpen, PagesTheKernelCannotWireAreNoBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(unwiredPages);
}

TEST(RawFacadeOpen, AYcbcrTargetStateOpensNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrTargetState);
}
