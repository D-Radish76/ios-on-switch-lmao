// SPDX-License-Identifier: MIT
// Render passes of depth and stencil attachments as the raw facade records them (raw_facade_harness.h): a Depth32Float
// texture beside a Stencil8 texture (array slices past 0 too), and an aspect whose store action the encoder decides
// (MTLStoreActionUnknown, then setDepthStoreAction: / setStencilStoreAction:), are the raw path's (no pass.ds refusal), and
// their command buffers complete on it, as does a depth cleared to NaN. A one-sample Depth16Unorm beside a Stencil8 is taken
// (its depth converted through a Depth32Float_Stencil8 by the pack's turn depth states, which this harness's pack lacks: it
// is not run here); a multisampled pair of them stays refused.
#include "raw_facade_harness.h"
#include "../raw/runtime/RawMetalInternal.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"

#include <gtest/gtest.h>
#include <cmath>

namespace {

using namespace harness;

id<MTLDevice> gDevice;

id<MTLTexture> zeta(MTLPixelFormat format)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:32 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    return [gDevice newTextureWithDescriptor:d];
}

id<MTLTexture> zetaChain(MTLPixelFormat format)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:100 height:60 mipmapped:YES];
    d.textureType = MTLTextureType2DArray;
    d.arrayLength = 6;
    d.mipmapLevelCount = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [gDevice newTextureWithDescriptor:d];
}

class FacadeZetaPass : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_TRUE(gDevice != nil);
    }

    // One pass of `pass` and nothing drawn, its encoder deciding the stores `decide` names: whether the raw path took it,
    // and the path its command buffer ran
    static std::pair<bool, std::string> run(MTLRenderPassDescriptor *pass, void (^decide)(id<MTLRenderCommandEncoder>))
    {
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
        if (decide)
            decide(e);
        [e endEncoding];
        const bool taken = !(((RMCommandBuffer *)cb)->_preds & 1ull << PredPassDepthStencil);
        [cb commit];
        [cb waitUntilCompleted];
        NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
        return {taken, cb.status == MTLCommandBufferStatusCompleted && !cb.error ? (path.UTF8String ?: "?") : "error"};
    }

    static MTLRenderPassDescriptor *pass(id<MTLTexture> depth, id<MTLTexture> stencil, MTLLoadAction load,
                                         MTLStoreAction store)
    {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.depthAttachment.texture = depth;
        p.depthAttachment.loadAction = load;
        p.depthAttachment.storeAction = store;
        p.stencilAttachment.texture = stencil;
        p.stencilAttachment.loadAction = load;
        p.stencilAttachment.storeAction = store;
        return p;
    }

    static std::map<uint32_t, uint32_t> methods(size_t first)
    {
        std::map<uint32_t, uint32_t> out;
        const auto &jobs = kernel_model::model.jobs;
        for (size_t j = first; j < jobs.size(); ++j) {
            const auto &words = jobs[j].words;
            for (size_t i = 0; i < words.size();) {
                const uint32_t header = words[i++], op = header >> 29, count = header >> 16 & 0x1fff,
                               subchannel = header >> 13 & 7, method = (header & 0xfff) << 2;
                if (op == 4) {
                    if (subchannel == raw::SUBC_3D)
                        out[method] = count;
                    continue;
                }
                for (uint32_t k = 0; k < count && i < words.size(); ++k, ++i)
                    if (subchannel == raw::SUBC_3D)
                        out[method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0)] = words[i];
            }
        }
        return out;
    }
};

TEST_F(FacadeZetaPass, MipAttachmentsBindTheirOwnZetaAddressAndExtent)
{
    const struct {
        MTLPixelFormat format;
        uint32_t zt, clear, bytes, stride, offset[4], pitch[4];
    } images[] = {
        {MTLPixelFormatDepth16Unorm, 0x13, 1, 2, 24576, {0, 16384, 20480, 21504}, {256, 128, 64, 64}},
        {MTLPixelFormatDepth32Float, 0x0a, 1, 4, 40960, {0, 28672, 36864, 38912}, {448, 256, 128, 64}},
        {MTLPixelFormatStencil8, 0x17, 2, 1, 12288, {0, 8192, 10240, 11264}, {128, 64, 64, 64}},
        {MTLPixelFormatDepth32Float_Stencil8, 0x19, 3, 8, 73728, {0, 53248, 67584, 71680}, {832, 448, 256, 128}},
    };

    const struct {
        uint32_t level, slice;
    } parts[] = {{0, 0}, {1, 2}, {2, 1}, {3, 5}};

    for (const auto &image : images) {
        auto texture = zetaChain(image.format);
        ASSERT_NE(texture, nil) << image.format;
        const auto &raw = ((RMTexture *)texture)->_raw;
        ASSERT_NE(raw.chain, nullptr);
        for (const auto &part : parts) {
            SCOPED_TRACE(::testing::Message() << image.format << '/' << part.level << '/' << part.slice);
            auto p = pass(image.format != MTLPixelFormatStencil8 ? texture : nil,
                          image.format == MTLPixelFormatStencil8 || image.format == MTLPixelFormatDepth32Float_Stencil8
                              ? texture
                              : nil,
                          MTLLoadActionClear, MTLStoreActionStore);
            p.depthAttachment.level = p.stencilAttachment.level = part.level;
            p.depthAttachment.slice = p.stencilAttachment.slice = part.slice;
            p.depthAttachment.clearDepth = 0.25;
            p.stencilAttachment.clearStencil = 0x5a;
            const size_t first = kernel_model::model.jobs.size();
            const auto [taken, path] = run(p, nil);
            EXPECT_TRUE(taken);
            EXPECT_EQ(path, "raw");
            const auto emitted = methods(first);
            const uint32_t required[] = {NVB197_SET_ZT_A,
                                         NVB197_SET_ZT_B,
                                         NVB197_SET_ZT_FORMAT,
                                         NVB197_SET_ZT_ARRAY_PITCH,
                                         NVB197_SET_ZT_SIZE_A,
                                         NVB197_SET_ZT_SIZE_B,
                                         NVB197_SET_ZT_SIZE_C,
                                         NVB197_SET_CLEAR_RECT_HORIZONTAL,
                                         NVB197_SET_CLEAR_RECT_VERTICAL,
                                         NVB197_SET_CLEAR_SURFACE_CONTROL,
                                         NVB197_CLEAR_SURFACE};
            for (const uint32_t method : required)
                ASSERT_NE(emitted.find(method), emitted.end()) << std::hex << method;
            const uint64_t address = uint64_t(emitted.at(NVB197_SET_ZT_A)) << 32 | emitted.at(NVB197_SET_ZT_B);
            EXPECT_EQ(address, raw.chain->va + image.offset[part.level] + uint64_t(part.slice) * image.stride);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_FORMAT), image.zt);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_ARRAY_PITCH), image.stride >> 2);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_A), image.pitch[part.level] / image.bytes);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_B), 60u >> part.level);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_C), 1u);
            EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_RECT_HORIZONTAL), (100u >> part.level) << 16);
            EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_RECT_VERTICAL), (60u >> part.level) << 16);
            EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_SURFACE_CONTROL), 0x10u);
            EXPECT_EQ(emitted.at(NVB197_CLEAR_SURFACE), image.clear);
        }
    }
}

TEST_F(FacadeZetaPass, CombinedAspectsRequireTheSameMipAndSlice)
{
    auto texture = zetaChain(MTLPixelFormatDepth32Float_Stencil8);
    ASSERT_NE(texture, nil);
    for (const bool mismatchLevel : {false, true}) {
        auto p = pass(texture, texture, MTLLoadActionClear, MTLStoreActionStore);
        p.depthAttachment.level = 1;
        p.stencilAttachment.level = mismatchLevel ? 2 : 1;
        p.depthAttachment.slice = 1;
        p.stencilAttachment.slice = mismatchLevel ? 1 : 2;
        auto cb = [[gDevice newCommandQueue] commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
        EXPECT_NE(((RMCommandBuffer *)cb)->_preds & 1ull << PredPassDepthStencil, 0u);
    }
}

TEST_F(FacadeZetaPass, MipAdmissionKeepsTheSeparateAspectAndChainBounds)
{
    auto depth = zetaChain(MTLPixelFormatDepth32Float), stencil = zetaChain(MTLPixelFormatStencil8);
    ASSERT_NE(depth, nil);
    ASSERT_NE(stencil, nil);
    const auto refused = [](MTLRenderPassDescriptor *p) {
        auto cb = [[gDevice newCommandQueue] commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
        EXPECT_NE(((RMCommandBuffer *)cb)->_preds & 1ull << PredPassDepthStencil, 0u);
    };
    auto separate = pass(depth, stencil, MTLLoadActionClear, MTLStoreActionStore);
    separate.depthAttachment.level = separate.stencilAttachment.level = 1;
    refused(separate);
    auto past = pass(depth, nil, MTLLoadActionClear, MTLStoreActionStore);
    past.depthAttachment.level = 4;
    refused(past);
    auto whole = pass(zeta(MTLPixelFormatDepth32Float), nil, MTLLoadActionClear, MTLStoreActionStore);
    whole.depthAttachment.level = 1;
    refused(whole);
}

TEST_F(FacadeZetaPass, ADepth32FloatBesideAStencil8IsTheRawPaths)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float), stencil = zeta(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil);
    for (MTLLoadAction load : {MTLLoadActionClear, MTLLoadActionLoad, MTLLoadActionDontCare})
        for (MTLStoreAction store : {MTLStoreActionStore, MTLStoreActionDontCare}) {
            const auto [taken, path] = run(pass(depth, stencil, load, store), nil);
            EXPECT_TRUE(taken) << load << "/" << store;
            EXPECT_EQ(path, "raw") << load << "/" << store;
        }
}

TEST_F(FacadeZetaPass, APairFromASlicePastZeroIsTheRawPaths)
{
    const auto array = [](MTLPixelFormat format) {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:32 mipmapped:NO];
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = 3;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget;
        return [gDevice newTextureWithDescriptor:d];
    };
    id<MTLTexture> depth = array(MTLPixelFormatDepth32Float), stencil = array(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil);
    for (NSUInteger from : {1, 2}) {
        MTLRenderPassDescriptor *p = pass(depth, stencil, MTLLoadActionLoad, MTLStoreActionStore);
        p.depthAttachment.slice = from;
        p.stencilAttachment.slice = 3 - from;
        const auto [taken, path] = run(p, nil);
        EXPECT_TRUE(taken) << from;
        EXPECT_EQ(path, "raw") << from;
    }
}

TEST_F(FacadeZetaPass, ADepth16UnormBesideAStencil8IsTakenAndATwoSampleOneIsNot)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth16Unorm), stencil = zeta(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil);
    const auto taken = [](MTLRenderPassDescriptor *p) {
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        [e endEncoding];
        return !(((RMCommandBuffer *)cb)->_preds & 1ull << PredPassDepthStencil);
    };
    EXPECT_TRUE(taken(pass(depth, stencil, MTLLoadActionLoad, MTLStoreActionStore)));
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth16Unorm
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.textureType = MTLTextureType2DMultisample;
    d.sampleCount = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> depth4 = [gDevice newTextureWithDescriptor:d];
    d.pixelFormat = MTLPixelFormatStencil8;
    id<MTLTexture> stencil4 = [gDevice newTextureWithDescriptor:d];
    ASSERT_TRUE(depth4 != nil && stencil4 != nil);
    EXPECT_FALSE(taken(pass(depth4, stencil4, MTLLoadActionLoad, MTLStoreActionStore)));
}

TEST_F(FacadeZetaPass, AStoreTheEncoderDecidesIsTheRawPaths)
{
    id<MTLTexture> combined = zeta(MTLPixelFormatDepth32Float_Stencil8);
    ASSERT_TRUE(combined != nil);
    for (MTLStoreAction decided : {MTLStoreActionStore, MTLStoreActionDontCare}) {
        const auto [taken, path] =
            run(pass(combined, combined, MTLLoadActionClear, MTLStoreActionUnknown), ^(id<MTLRenderCommandEncoder> e) {
                [e setDepthStoreAction:decided];
                [e setStencilStoreAction:decided];
            });
        EXPECT_TRUE(taken) << decided;
        EXPECT_EQ(path, "raw") << decided;
    }
}

TEST_F(FacadeZetaPass, ANanClearDepthIsTheRawPaths)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float);
    ASSERT_TRUE(depth != nil);
    MTLRenderPassDescriptor *p = pass(depth, nil, MTLLoadActionClear, MTLStoreActionStore);
    p.depthAttachment.clearDepth = NAN;
    const auto [taken, path] = run(p, nil);
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "raw");
}

}  // namespace
