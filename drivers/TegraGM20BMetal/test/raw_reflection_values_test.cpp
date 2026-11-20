// SPDX-License-Identifier: MIT
// The raw facade's reflection objects (raw/runtime/RawMetalReflectionObjects.mm) of a blob answer as this Mac's Metal
// answers the same state, laid out as the translator lays out a reflection (metal_reflection_blob.h): every binding
// type (buffers of a vector, a struct, a union of two anonymous structs, a scalar; textures alone and in arrays,
// depth, written; samplers alone and in arrays; threadgroup memory; a tile function's imageblock) and type class,
// compute, render and tile, selector by selector and in which selectors each responds to. A blob that is not one whole
// gives no reflection.
#include "../raw/runtime/RawMetalReflection.h"
#include "metal_reflection_blob.h"
#include "reflection_blob_writer.h"
#include "reflection_compare.h"

#import <objc/message.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr MTLPipelineOption kReflected = MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;

constexpr char kSource[] = R"(
#include <metal_stdlib>
using namespace metal;
struct S { float4 v; float a[4]; };
struct Args { texture2d<float> t; sampler s; device float *p; array<texture2d<float>, 2> ts; };
struct Union { union { struct { float a; float b; }; struct { int c; }; }; };
kernel void k(device float4 *b4 [[buffer(0)]], device S *bs [[buffer(1)]], constant float &cf [[buffer(2)]],
              device uint *bu [[buffer(3)]], device half *unused [[buffer(4)]], device Args &ab [[buffer(5)]],
              texture2d<float> t [[texture(0)]], array<texture2d<float>, 4> ta [[texture(1)]],
              texture2d_array<float> tarr [[texture(5)]], depth2d<float> d [[texture(6)]],
              array<depth2d<float>, 3> dc [[texture(7)]], texture2d<half, access::write> w [[texture(10)]],
              sampler s [[sampler(0)]], array<sampler, 3> sa [[sampler(1)]], array<sampler, 2> sc [[sampler(4)]],
              threadgroup float *tg [[threadgroup(0)]], threadgroup float4 *tg4 [[threadgroup(1)]],
              constant Union &un [[buffer(6)]], uint id [[thread_position_in_grid]]) {
    float x = b4[id].x + bs[id].a[1] + cf + float(bu[id]) + t.sample(s, float2(0)).x + ta[3].sample(sa[2], float2(0)).x
            + tarr.sample(s, float2(0), 1).x + d.sample(sc[1], float2(0)) + dc[2].sample(sc[0], float2(0))
            + ab.p[id] + ab.t.sample(ab.s, float2(0)).x + ab.ts[1].read(uint2(0)).x + un.a + float(un.c);
    tg[id] = x;
    tg4[id] = float4(x);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    w.write(half4(tg4[0]), uint2(id, 0));
    b4[id] = float4(tg[0]);
}
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(device const float4 *pos [[buffer(0)]], constant S &u [[buffer(1)]], texture2d<float> vt [[texture(0)]],
            uint vid [[vertex_id]]) {
    V o; o.p = pos[vid] + u.v + vt.read(uint2(0)); o.uv = float2(0); return o;
}
fragment float4 fs(V in [[stage_in]], texture2d<float> ft [[texture(0)]], array<texture2d<float>, 2> fa [[texture(1)]],
                   sampler fsm [[sampler(0)]], array<sampler, 2> fsa [[sampler(1)]],
                   constant float4 &tint [[buffer(0)]]) {
    return ft.sample(fsm, in.uv) + fa[1].sample(fsa[1], in.uv) * tint;
}
struct TileOut { half4 c [[color(0)]]; };
kernel void tk(imageblock<TileOut, imageblock_layout_implicit> ib, device float4 *tb [[buffer(0)]],
               texture2d<float> tt [[texture(0)]], threadgroup float *ttg [[threadgroup(0)]],
               ushort2 lid [[thread_position_in_threadgroup]]) {
    ttg[0] = tt.read(uint2(0)).x + tb[0].x;
    TileOut o = ib.read(lid); o.c += half4(ttg[0]); ib.write(o, lid);
}
)";

id<MTLLibrary> library(id<MTLDevice> device)
{
    NSError *error = nil;
    id<MTLLibrary> made = [device newLibraryWithSource:@(kSource) options:nil error:&error];
    EXPECT_TRUE(made != nil) << (error ? error.localizedDescription.UTF8String : "");
    return made;
}

id rawOf(const std::string &bytes, bool compute)
{
    return rmReflectionOf(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), compute);
}

// `metal`'s stages as a render state's blob: vertex, fragment, tile, and no object or mesh stage
std::string renderBlob(MTLRenderPipelineReflection *metal)
{
    metal_reflection::Table table;
    NSArray *stages[] = {metal.vertexBindings, metal.fragmentBindings, metal.tileBindings, @[], @[]};
    for (NSArray *stage : stages)
        table.stage(stage);
    return table.blob();
}

// m2v's table of a buffer binding whose struct holds a float4 and an array of depth textures, through a pointer too; a
// texture binding; a threadgroup binding (reflection_blob_writer.h of)
struct Table {
    std::vector<NmbReflectionNode> nodes;
    std::vector<uint32_t> edges = {3, 4, 7, 8, 9};

    Table()
    {
        const auto node = [](uint32_t kind, const char *name, uint32_t flags, std::initializer_list<uint64_t> values,
                             std::initializer_list<uint32_t> children, uint32_t first = 0, uint32_t count = 0) {
            NmbReflectionNode n = {};
            n.kind = kind;
            n.name = name;
            n.flags = flags;
            std::copy(values.begin(), values.end(), n.values);
            std::fill(std::begin(n.children), std::end(n.children), UINT32_MAX);
            std::copy(children.begin(), children.end(), n.children);
            n.first = first;
            n.count = count;
            return n;
        };
        const uint32_t none = UINT32_MAX;
        nodes = {
            node(0, nullptr, 0, {}, {}, 2, 3),
            node(6, nullptr, 1, {MTLDataTypeFloat, MTLTextureType2D, MTLBindingAccessReadOnly}, {}),
            node(4, nullptr, 0, {MTLDataTypeTexture, 4, 8, 1}, {none, none, 1}),
            node(3, "colour", 0, {MTLDataTypeFloat4, 0, 0}, {}),
            node(3, "depths", 0, {MTLDataTypeArray, 16, 1}, {none, 2}),
            node(2, nullptr, 0, {}, {}, 0, 2),
            node(5, nullptr, 1, {MTLDataTypeStruct, MTLBindingAccessReadWrite, 16, 48}, {5}),
            node(1, "uniforms", 3, {MTLBindingTypeBuffer, MTLBindingAccessReadOnly, 3, 0, 0, 16, 48, MTLDataTypeStruct},
                 {5, none, none, 6}),
            node(1, "shadow", 5,
                 {MTLBindingTypeTexture, MTLBindingAccessReadOnly, 1, 0, 0, MTLTextureType2D, MTLDataTypeFloat, 1}, {}),
            node(1, "scratch", 0, {MTLBindingTypeThreadgroupMemory, MTLBindingAccessReadWrite, 0, 0, 0, 16, 256}, {}),
        };
    }
};

}  // namespace

TEST(RawReflection, AComputeStatesBindingsOfEveryTypeAnswerAsMetals)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_TRUE(device != nil);
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = [library(device) newFunctionWithName:@"k"];
    MTLComputePipelineReflection *metal = nil;
    NSError *error = nil;
    id<MTLComputePipelineState> state =
        [device newComputePipelineStateWithDescriptor:d options:kReflected reflection:&metal error:&error];
    ASSERT_TRUE(state != nil) << (error ? error.localizedDescription.UTF8String : "");
    metal_reflection::Table table;
    table.stage(metal.bindings);
    id raw = rawOf(table.blob(), true);
    ASSERT_TRUE(raw != nil);
    EXPECT_TRUE([raw isKindOfClass:[MTLComputePipelineReflection class]]);
    reflection_compare::expectSameList(metal.bindings, [raw bindings], "bindings");
    reflection_compare::expectSameList(metal.arguments, [raw arguments], "arguments");
    EXPECT_EQ(rawOf(table.blob(), false), nil) << "a compute blob read as a render one";
}

TEST(RawReflection, ARenderStatesAndATileStatesStagesAnswerAsMetals)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = library(device);
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [functions newFunctionWithName:@"vs"];
    d.fragmentFunction = [functions newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    MTLRenderPipelineReflection *metal = nil;
    NSError *error = nil;
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:d options:kReflected reflection:&metal error:&error];
    ASSERT_TRUE(state != nil) << (error ? error.localizedDescription.UTF8String : "");
    id raw = rawOf(renderBlob(metal), false);
    ASSERT_TRUE(raw != nil);
    EXPECT_TRUE([raw isKindOfClass:[MTLRenderPipelineReflection class]]);
    reflection_compare::expectSameRender(metal, raw, "render");
    for (NSString *empty : @[ @"objectBindings", @"meshBindings" ])
        EXPECT_EQ([((NSArray * (*)(id, SEL)) objc_msgSend)(raw, NSSelectorFromString(empty)) count], 0u)
            << empty.UTF8String;
    EXPECT_EQ(rawOf(renderBlob(metal), true), nil) << "a render blob read as a compute one";

    MTLTileRenderPipelineDescriptor *t = [MTLTileRenderPipelineDescriptor new];
    t.tileFunction = [functions newFunctionWithName:@"tk"];
    t.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    t.threadgroupSizeMatchesTileSize = YES;
    MTLRenderPipelineReflection *tile = nil;
    id<MTLRenderPipelineState> tileState =
        [device newRenderPipelineStateWithTileDescriptor:t options:kReflected reflection:&tile error:&error];
    ASSERT_TRUE(tileState != nil) << (error ? error.localizedDescription.UTF8String : "");
    reflection_compare::expectSameRender(tile, rawOf(renderBlob(tile), false), "tile");
}

TEST(RawReflection, ABlobThatIsNotOneWholeGivesNone)
{
    const Table table;
    const std::string whole = reflection_blob::of(table.nodes, table.edges, raw::kReflectionComputeStages);
    ASSERT_NE(rawOf(whole, true), nil);
    EXPECT_EQ(rawOf(whole.substr(0, whole.size() - 1), true), nil);
    std::string version = whole;
    version[4] = 2;
    EXPECT_EQ(rawOf(version, true), nil);
    Table cycle;
    cycle.nodes[2].children[2] = 7;  // the array names a node after it
    EXPECT_EQ(rawOf(reflection_blob::of(cycle.nodes, cycle.edges, 1), true), nil);
    Table kinds;
    kinds.nodes[4].children[1] = 1;  // a member's array is a texture reference
    EXPECT_EQ(rawOf(reflection_blob::of(kinds.nodes, kinds.edges, 1), true), nil);
}
