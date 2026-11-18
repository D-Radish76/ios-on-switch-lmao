// SPDX-License-Identifier: MIT
// Extended-range textures a pass's draws write or sample, on the raw path with nothing of it on m2v. A pass whose draws write
// more two-plane RGB10A8_2P_XR10 textures than raw::PassDesc holds inline (each draw writes 16 slots and different draws
// different textures) takes an image for every one and renders, no image's backing tag one another kind of scratch of the
// pass holds. A texel buffer a draw samples is converted into its stand-in first: its codes copied in order into the
// stand-in's rows by the copy engine, then the conversion's pass (RawMetalRangeStep.mm standIns).
#include "raw_facade_pass_scene.h"
#import "../raw/runtime/RawMetalXrPlanes.h"
#include "../raw/runtime/raw_attachment_target.h"
#include "../raw/runtime/raw_format_standin.h"
#include "../raw/runtime/raw_push.h"

#include <set>

namespace {

using namespace pass_scene;

void writesEverySlot(const raw::CompileRequest &request, raw::RuntimeRecord &record)
{
    raw::PackPipeline &pp = record.pipeline;
    raw::setEntryCount(pp, raw::kWrittenSlots);
    for (uint32_t slot = 0; slot < raw::kWrittenSlots; slot++) {
        pp.entry[slot] = {uint16_t(16 * slot), raw::EntryTexture, 2, uint8_t(slot), raw::SamplerNone, 0, 0};
        pp.writtenFormat[slot] = 550;
    }
    pp.setBytes = 16 * raw::kWrittenSlots;
    pp.memoryEntries = (1u << raw::kWrittenSlots) - 1;
    pp.xrSlots = request.variant.xrSlots;
}

// A 64x64 RGB10A8_2P_XR10 texture over the two planes of a surface of its own
id<MTLTexture> twoPlaneTexture(id<MTLDevice> device)
{
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @64,
            (id)kIOSurfacePlaneHeight : @64,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(64 * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 64 * 64 * 4),
            (id)kIOSurfacePlaneSize : @(64 * 64 * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(64 * 64 * 5),
        (id)kIOSurfacePlaneInfo : planes
    });
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550) width:64 height:64 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> t = surface ? [device newTextureWithDescriptor:d iosurface:surface plane:0] : nil;
    if (surface)
        CFRelease(surface);
    return t;
}

// `count` two-plane textures written in one pass: draws of 16 written slots each, the last one's first slots the rest
void twoPlaneTexturesWrittenInOnePass(uint32_t count)
{
    RenderCompiler compiler;
    compiler.shapes = writesEverySlot;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "xr-images",
                                    writePack(packKey(MTLPixelFormatRGBA16Float), {}, {}, true, "",
                                              {{"raw.xr.xr_planes_merge_frag", MTLPixelFormatRG32Uint},
                                               {"raw.xr.xr_planes_split_frag", MTLPixelFormatR32Uint}}),
                                    device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil && ((RMPipeline *)state)->_raw);
    ASSERT_EQ(((RMPipeline *)state)->_raw->writtenSlots, 0xffff);
    std::vector<id<MTLTexture>> written;
    for (uint32_t k = 0; k < count; k++) {
        written.push_back(twoPlaneTexture(device));
        ASSERT_TRUE(written.back() != nil) << k;
    }
    id<MTLTexture> colour = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64);
    ASSERT_TRUE(colour != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = colour;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    // The variant reading the 16 slots in their planes is asked once, at the first bind of all of them (a later group
    // leaves every slot holding a two-plane texture: the same forms), and taken from the first draw on. Bound one by one,
    // each bind would ask the variant of the slots bound so far, a burst the harness's compiler does not answer.
    raw::RenderVariant pairs;
    pairs.xrSlots = 0xffff;
    __block bool asked = false, readied = false;
    id<MTLCommandBuffer> cb = run(device, p, ^(id<MTLRenderCommandEncoder> e) {
      [e setRenderPipelineState:state];
      for (size_t first = 0; first < written.size(); first += raw::kWrittenSlots) {
          const size_t n = std::min<size_t>(raw::kWrittenSlots, written.size() - first);
          id<MTLTexture> __unsafe_unretained group[raw::kWrittenSlots];
          for (size_t k = 0; k < n; k++)
              group[k] = written[first + k];
          [e setFragmentTextures:group withRange:NSMakeRange(0, n)];
          if (!first) {
              raw::Variant<raw::Pipeline> *variant = variantOf(state, pairs);
              asked = variant != nullptr;
              readied = asked && ready(*variant);
          }
          [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      }
    });
    EXPECT_TRUE(asked);
    EXPECT_TRUE(readied) << "the draws read the textures in their planes, through no stand-in";
    expectRaw(device, cb);
}

void samplesTextureZero(const raw::CompileRequest &, raw::RuntimeRecord &record)
{
    raw::PackPipeline &pp = record.pipeline;
    raw::setEntryCount(pp, 1);
    pp.entry[0] = {0, raw::EntryTexture, 2, 0, raw::SamplerNone, 0, 0};
    pp.setBytes = 16;
    pp.memoryEntries = 1;
}

// A copy engine launch's destination, or a pass's colour target 0, in the order the jobs from `first` on pushed them
struct Event {
    bool copy;
    uint64_t address;
};

std::vector<Event> events(size_t first)
{
    constexpr uint32_t kTargetA = 0x0800, kTargetB = 0x0804, kTargetFormat = 0x0810, kOutUpper = 0x0408,
                       kOutLower = 0x040c, kLaunchDma = 0x0300;
    std::vector<Event> out;
    uint64_t target = 0, to = 0;
    const auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == raw::SUBC_3D && m == kTargetA)
            target = (target & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == raw::SUBC_3D && m == kTargetB)
            target = (target & ~0xffffffffull) | v;
        else if (subc == raw::SUBC_3D && m == kTargetFormat && v)
            out.push_back({false, target});
        else if (subc == raw::SUBC_COPY && m == kOutUpper)
            to = (to & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == raw::SUBC_COPY && m == kOutLower)
            to = (to & ~0xffffffffull) | v;
        else if (subc == raw::SUBC_COPY && m == kLaunchDma)
            out.push_back({true, to});
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            if (op == 4) {
                method(subc, m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(subc, op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i]);
        }
    }
    return out;
}

size_t indexOf(const std::vector<Event> &e, bool copy, uint64_t address)
{
    for (size_t i = 0; i < e.size(); ++i)
        if (e[i].copy == copy && e[i].address == address)
            return i;
    return SIZE_MAX;
}

void aSampledTexelBufferIsConvertedFromItsCodes()
{
    RenderCompiler compiler;
    compiler.shapes = samplesTextureZero;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library =
        served(compiler, "xr-texel-buffer",
               writePack(packKey(MTLPixelFormatRGBA16Float), {}, {}, true, "",
                         {{raw::xrConversionOf(552), MTLPixelFormatRGBA16Float}}),
               device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil && ((RMPipeline *)state)->_raw);
    constexpr NSUInteger kTexels = 300;
    id<MTLBuffer> buffer = [device newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormat(552)
                                                                                     width:kTexels
                                                                           resourceOptions:MTLResourceStorageModeShared
                                                                                     usage:MTLTextureUsageShaderRead];
    id<MTLTexture> texels = [buffer newTextureWithDescriptor:d offset:0 bytesPerRow:kTexels * 8];
    ASSERT_TRUE(texels != nil && ((RMTexture *)texels)->_rawOk);
    const raw::Texture *standIn = ((RMTexture *)texels)->_raw.standIn;
    ASSERT_TRUE(standIn && standIn->luma) << "a stand-in of rows and the image its codes are copied into";
    id<MTLTexture> colour = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64);
    ASSERT_TRUE(colour != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = colour;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p, ^(id<MTLRenderCommandEncoder> e) {
                  [e setRenderPipelineState:state];
                  [e setFragmentTexture:texels atIndex:0];
                  [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
              }));
    const std::vector<Event> e = events(first);
    const size_t copied = indexOf(e, true, standIn->luma->va), converted = indexOf(e, false, standIn->va),
                 drawn = indexOf(e, false, ((RMTexture *)colour)->_raw.va);
    ASSERT_NE(copied, SIZE_MAX) << "the codes copied into the stand-in's image";
    ASSERT_NE(converted, SIZE_MAX) << "the conversion's pass into the stand-in";
    ASSERT_NE(drawn, SIZE_MAX);
    EXPECT_LT(copied, converted);
    EXPECT_LT(converted, drawn);
}

}  // namespace

TEST(RawFacadeXrImages, ASampledTexelBufferIsConvertedFromItsCodesFirst)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSampledTexelBufferIsConvertedFromItsCodes);
}

TEST(RawFacadeXrImages, TwentyFiveTwoPlaneTexturesWrittenInAPassRender)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        twoPlaneTexturesWrittenInOnePass(raw::PassDesc::kWrittenPlanes + 1);
    });
}

TEST(RawFacadeXrImages, FortyTwoPlaneTexturesWrittenInAPassRender)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        twoPlaneTexturesWrittenInOnePass(40);
    });
}

TEST(RawFacadeXrImages, NoImageTagIsAnotherScratchsOrAnotherImages)
{
    std::set<uint32_t> tags;
    for (uint32_t k = 0; k < 64; k++) {
        const uint32_t tag = xrPlanesImageTag(k);
        EXPECT_TRUE(tags.insert(tag).second) << "image " << k << " shares tag " << tag;
        EXPECT_FALSE(tag >= raw::kPlaneImageTag && tag < raw::kPlaneImageTag + 8) << k;
        EXPECT_FALSE(tag >= 48 && tag < kXrPlanesImageTag) << k << ": a turn copy's, reach's, conversion's or load's";
        EXPECT_FALSE(tag >= raw::kFormatStandInTag && tag < raw::kFormatStandInTag + 16) << k << ": a format stand-in's";
    }
}
