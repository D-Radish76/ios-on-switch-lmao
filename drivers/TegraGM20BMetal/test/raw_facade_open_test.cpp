// SPDX-License-Identifier: MIT
// What no process opens its m2v device for: a library's own answers, a library Metal would not load, memory the kernel
// has none of, pages it cannot wire, and a YCbCr target state raw draws. Each answer is the raw device's or Metal's nil.
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_ycbcr_store.h"
#include "bazel/testing/fresh_process.h"

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
    EXPECT_EQ([device newBufferWithBytesNoCopy:pages length:length options:MTLResourceStorageModeShared deallocator:nil],
              nil);
    EXPECT_NE([device newBufferWithBytesNoCopy:pages length:length options:MTLResourceStorageModeShared deallocator:nil],
              nil)
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

}  // namespace

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
