// SPDX-License-Identifier: MIT
// The raw facade's reflection (raw/runtime/RawMetalReflection.mm) of a blob answers every selector Metal's reflection
// classes have exactly as m2v's frontend answered from the same node table (NativeComputeReflection.h computeReflection,
// over its compute artifact's NmbReflectionNode table): every kind of node, nested, named and not. A blob that is not
// one whole (a child after its parent, a name past the names, another version) gives no reflection.
#include "../raw/runtime/RawMetalReflection.h"
#include "NativeComputeReflection.h"
#include "reflection_blob_writer.h"

#import <objc/message.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace {

// m2v's table: a buffer binding whose struct holds a float4 and an array of depth textures, through a pointer too; a
// texture binding; a threadgroup binding
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
            node(1, "uniforms", 3,
                 {MTLBindingTypeBuffer, MTLBindingAccessReadOnly, 3, 0, 0, 16, 48, MTLDataTypeStruct}, {5, none, none, 6}),
            node(1, "shadow", 5,
                 {MTLBindingTypeTexture, MTLBindingAccessReadOnly, 1, 0, 0, MTLTextureType2D, MTLDataTypeFloat, 1}, {}),
            node(1, "scratch", 0, {MTLBindingTypeThreadgroupMemory, MTLBindingAccessReadWrite, 0, 0, 0, 16, 256}, {}),
        };
    }
};

const Table kTable;

}  // namespace

extern "C" bool nmb_compute_artifact_reflection(const NmbComputeArtifact *, const NmbReflectionNode **nodes,
                                                size_t *node_count, const uint32_t **edges, size_t *edge_count)
{
    *nodes = kTable.nodes.data();
    *node_count = kTable.nodes.size();
    *edges = kTable.edges.data();
    *edge_count = kTable.edges.size();
    return true;
}

namespace {

constexpr const char *kCounts[] = {"type",
                                   "access",
                                   "index",
                                   "bufferAlignment",
                                   "bufferDataSize",
                                   "bufferDataType",
                                   "threadgroupMemoryAlignment",
                                   "threadgroupMemoryDataSize",
                                   "textureType",
                                   "textureDataType",
                                   "arrayLength",
                                   "dataType",
                                   "offset",
                                   "argumentIndex",
                                   "elementType",
                                   "stride",
                                   "argumentIndexStride",
                                   "alignment",
                                   "dataSize"};
constexpr const char *kFlags[] = {"isUsed", "isActive", "isArgument", "isDepthTexture", "elementIsArgumentBuffer"};
constexpr const char *kObjects[] = {"bufferStructType",   "bufferPointerType",          "structType",
                                    "arrayType",          "textureReferenceType",       "pointerType",
                                    "elementStructType",  "elementArrayType",           "elementTextureReferenceType",
                                    "elementPointerType"};

void expectSame(id m2v, id raw, const std::string &path);

void expectSameList(NSArray *m2v, NSArray *raw, const std::string &path)
{
    ASSERT_EQ(m2v.count, raw.count) << path;
    for (NSUInteger i = 0; i < m2v.count; i++)
        expectSame(m2v[i], raw[i], path + "[" + std::to_string(i) + "]");
}

void expectSame(id m2v, id raw, const std::string &path)
{
    ASSERT_EQ(m2v == nil, raw == nil) << path;
    if (!m2v)
        return;
    EXPECT_TRUE([[m2v name] isEqual:[raw name]] || (![m2v name] && ![raw name])) << path;
    for (const char *s : kCounts)
        EXPECT_EQ(((NSUInteger(*)(id, SEL))objc_msgSend)(m2v, sel_registerName(s)),
                  ((NSUInteger(*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)))
            << path << " " << s;
    for (const char *s : kFlags)
        EXPECT_EQ(((BOOL(*)(id, SEL))objc_msgSend)(m2v, sel_registerName(s)),
                  ((BOOL(*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)))
            << path << " " << s;
    for (Class c : {[MTLArgument class], [MTLStructType class], [MTLStructMember class], [MTLArrayType class],
                    [MTLPointerType class], [MTLTextureReferenceType class], [MTLType class]})
        EXPECT_EQ([m2v isKindOfClass:c], [raw isKindOfClass:c]) << path << " " << class_getName(c);
    for (Protocol *p : {@protocol(MTLBinding), @protocol(MTLBufferBinding), @protocol(MTLTextureBinding),
                        @protocol(MTLThreadgroupBinding)})
        EXPECT_EQ([m2v conformsToProtocol:p], [raw conformsToProtocol:p]) << path << " " << protocol_getName(p);
    for (const char *s : kObjects)
        expectSame(((id(*)(id, SEL))objc_msgSend)(m2v, sel_registerName(s)),
                   ((id(*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)), path + "." + s);
    expectSameList([m2v members], [raw members], path + ".members");
    for (id member in [m2v members])
        expectSame([m2v memberByName:[member name]], [raw memberByName:[member name]], path + ".memberByName");
}

std::string blob(uint32_t stages)
{
    return reflection_blob::of(kTable.nodes, kTable.edges, stages);
}

id rawOf(const std::string &bytes, bool compute)
{
    return rmReflectionOf(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), compute);
}

}  // namespace

TEST(RawReflection, AComputeStatesAnswersAsM2VsFromTheSameTable)
{
    id m2v = computeReflection(nullptr);
    id raw = rawOf(blob(raw::kReflectionComputeStages), true);
    ASSERT_NE(m2v, nil);
    ASSERT_NE(raw, nil);
    EXPECT_TRUE([raw isKindOfClass:[MTLComputePipelineReflection class]]);
    expectSameList([m2v bindings], [raw bindings], "bindings");
    expectSameList([m2v arguments], [raw arguments], "arguments");
    EXPECT_EQ([[raw bindings] count], 3u);
}

TEST(RawReflection, ARenderStatesStagesAreItsBlobsStages)
{
    id m2v = computeReflection(nullptr);
    id raw = rawOf(blob(raw::kReflectionRenderStages), false);
    ASSERT_NE(raw, nil);
    EXPECT_TRUE([raw isKindOfClass:[MTLRenderPipelineReflection class]]);
    expectSameList([m2v bindings], [raw vertexBindings], "vertexBindings");
    expectSameList([m2v arguments], [raw vertexArguments], "vertexArguments");
    for (NSString *empty : @[ @"fragmentBindings", @"tileBindings", @"objectBindings", @"meshBindings" ])
        EXPECT_EQ([((NSArray * (*)(id, SEL)) objc_msgSend)(raw, NSSelectorFromString(empty)) count], 0u)
            << empty.UTF8String;
    EXPECT_EQ(rawOf(blob(raw::kReflectionRenderStages), true), nil) << "a render blob read as a compute one";
}

TEST(RawReflection, ABlobThatIsNotOneWholeGivesNone)
{
    const std::string whole = blob(raw::kReflectionComputeStages);
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
