// SPDX-License-Identifier: MIT
// A state's layered storage form drawn in a one-layer pass that loads a linear texture over a buffer (HW Test
// metal.native.layered_fetch, one_layer_linear): the pass renders the texture through a block-linear backing, whose 2D
// array storage view the fetch reads, also where it follows a clear pass of the texture in the same command buffer,
// whose begin laid the texture out as a pitch target (RawMetalRangeSeal.mm storageLaidOut: no fusion into that pass).
#include "raw_facade_variant_compiler.h"
#include "bazel/testing/fresh_process.h"

#include <unistd.h>

namespace {

using namespace harness;

constexpr uint32_t kR32UintWord0 = 0x6014920f;  // the storage view's word 0: R32_UINT, a 4-byte texel as RGBA8's

// The record of a state fetching colour 0 through its layered storage form (raw_device_layered_fetch_test.cpp
// fetching): its vertex program writes Layer, the pass's length goes into push word 2
void layeredStorageForm(const raw::CompileRequest &, raw::RuntimeRecord &record)
{
    raw::PackPipeline &pp = record.pipeline;
    raw::setEntryCount(pp, 3);
    pp.entry[2] = {32, raw::EntryFetch, 2, 0, raw::SamplerNone, 0, 0};
    pp.setBytes = 48;
    pp.memoryEntries = 7;
    pp.flags |= raw::PipelineFetches | raw::PipelineStorageMetadata;
    pp.storageMask = pp.storageWriteMask = 1;
    pp.storageSamples = 1;
    pp.storageTic[0] = kR32UintWord0;
    pp.arrayLengthWord = 3;
    pp.pushMask = 1u << 2;
    record.programs[0].image[53] |= 2;
    record.blob = {0x1224, 1, 0x1a00, 0};
}

id<MTLLibrary> served(RenderCompiler &compiler, const std::string &name, const std::string &pack,
                      id<MTLDevice> __strong &device)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory) || !(device = open(pack)))
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    raw_metal_complete_states(cache.c_str(), nullptr);
    return [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
}

MTLRenderPassDescriptor *pass(id<MTLTexture> t, MTLLoadAction load)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = load;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    return p;
}

void layeredFetchIntoALinearTextureAfterItsClear()
{
    RenderCompiler compiler;
    compiler.shapes = layeredStorageForm;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "fused-storage", writePack(packKey(MTLPixelFormatBGRA8Unorm)), device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatRGBA8Unorm) error:nil];
    ASSERT_TRUE(state != nil);
    __unsafe_unretained RMPipeline *pipeline = (RMPipeline *)state;
    ASSERT_TRUE(pipeline->_raw);
    ASSERT_EQ(pipeline->_raw->storageBits, 1);
    ASSERT_TRUE(raw::layeredRecord(pipeline->_raw->rec->arrayLengthWord));
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLBuffer> rows = [device newBufferWithLength:32 * 128 options:MTLResourceStorageModeShared];
    id<MTLTexture> linear = [rows newTextureWithDescriptor:d offset:0 bytesPerRow:128];
    ASSERT_TRUE(linear != nil);
    ASSERT_FALSE(((RMTexture *)linear)->_raw.blockLinear);
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    [[commands renderCommandEncoderWithDescriptor:pass(linear, MTLLoadActionClear)] endEncoding];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass(linear, MTLLoadActionLoad)];
    [encoder setRenderPipelineState:state];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:1];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(commands, sel_registerName("switchRawPath"));
    EXPECT_EQ(std::string(path.UTF8String ?: ""), "raw");
}

}  // namespace

TEST(RawFacadeFusedStorage, ALayeredFetchIntoALinearTextureAfterItsClearIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(layeredFetchIntoALinearTextureAfterItsClear);
}
