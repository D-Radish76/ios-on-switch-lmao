// SPDX-License-Identifier: MIT
// A function's functionConstantsDictionary, vertexAttributes and stageInputAttributes come from its metallib's public metadata,
// with no m2v device opened. testdata/function_tags/probe.metallib is Xcode's metal 32023.922 build of probe.metal; the answers
// below are what an M-series Mac's Metal gives for it (the same as the tag reader gave for all 10753 functions of the macOS
// 27.2 MPS, MPSGraph, Core Image, CoreUI, Vision, RenderBox and QuartzCore metallibs).
#include "raw_facade_harness.h"
#include "bazel/testing/fresh_process.h"

#include <fstream>
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

// name:type:index:(r)equired or (o)ptional, in name order
std::string constants(id<MTLFunction> f)
{
    std::string out;
    for (NSString *name in [f.functionConstantsDictionary.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        MTLFunctionConstant *c = f.functionConstantsDictionary[name];
        out += std::string(c.name.UTF8String) + ":" + std::to_string(c.type) + ":" + std::to_string(c.index) +
               (c.required ? ":r " : ":o ");
    }
    return out;
}

// name:index:type:(a)ctive,(p)atch data,(c)ontrol-point data; "nil" for nil
std::string attributes(NSArray *list)
{
    if (!list)
        return "nil";
    std::string out;
    for (MTLAttribute *a in list)
        out += std::string(a.name.UTF8String) + ":" + std::to_string(a.attributeIndex) + ":" +
               std::to_string(a.attributeType) + ":" + (a.active ? "a" : "-") + (a.patchData ? "p" : "-") +
               (a.patchControlPointData ? "c" : "-") + " ";
    return out;
}

struct Measured {
    const char *function, *constants, *vertexAttributes, *stageInputAttributes;
};

constexpr Measured kProbe[] = {
    {"v_attrs", "hasNormal:53:4:o scale:3:1:r ", "position:0:6:a-- uv:2:4:a-- normal:5:5:a-- colour:7:19:--- ",
     "position:0:6:a-- uv:2:4:a-- normal:5:5:a-- colour:7:19:--- "},
    {"v_patch", "", "p:0:6:a-c extra:3:6:ap- ", "p:0:6:a-c extra:3:6:ap- "},
    {"f_tint", "useTint:53:0:o ", "nil", "nil"},
    {"k_stage", "mode:29:3:r ", "nil", "p:1:4:a-- id:4:33:--- "},
    {"k_plain", "", "nil", "nil"},
};

void measuredAnswers()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const char *probe = getenv("SWITCH_FUNCTION_TAGS_PROBE");
    ASSERT_TRUE(probe != nullptr);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithFile:@(probe) error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    for (const Measured &m : kProbe) {
        id<MTLFunction> f = [library newFunctionWithName:@(m.function)];
        ASSERT_TRUE(f != nil) << m.function;
        EXPECT_EQ(constants(f), m.constants) << m.function;
        EXPECT_EQ(attributes(f.vertexAttributes), m.vertexAttributes) << m.function;
        EXPECT_EQ(attributes(f.stageInputAttributes), m.stageInputAttributes) << m.function;
    }
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    const int mode = 2;
    [values setConstantValue:&mode type:MTLDataTypeInt atIndex:3];
    id<MTLFunction> specialized = [library newFunctionWithName:@"k_plain" constantValues:values error:&error];
    ASSERT_TRUE(specialized != nil) << text(error);
    EXPECT_EQ(constants(specialized), "") << "a function made with constant values answers none";
    EXPECT_EQ(attributes(specialized.stageInputAttributes), "nil");
    expectNoM2V(device);
}

std::vector<uint8_t> tagged(const char *fourcc, const std::vector<uint8_t> &value)
{
    std::vector<uint8_t> out;
    made_metallib::tag(out, fourcc, value.data(), uint16_t(value.size()));
    return out;
}

// A u16 count, then each entry's name and `fields`
std::vector<uint8_t> entries(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &list)
{
    std::vector<uint8_t> out = {uint8_t(list.size()), 0};
    for (const auto &[name, fields] : list) {
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(0);
        out.insert(out.end(), fields.begin(), fields.end());
    }
    return out;
}

// What m2v refused as not measured is answered as it answered: no constants, nil attributes; with no m2v device
void unmeasuredTags()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const std::vector<uint8_t> flagTwo = tagged("CNST", entries({{"k", {29, 0, 0, 2}}}));
    std::vector<uint8_t> highBit = tagged("VATT", entries({{"a", {0x00, 0x10}}}));
    const std::vector<uint8_t> types = tagged("VATY", {1, 0, 6});
    highBit.insert(highBit.end(), types.begin(), types.end());
    std::vector<uint8_t> typeless = tagged("VATT", entries({{"b", {0x01, 0x80}}}));
    const std::vector<uint8_t> unknownType = tagged("VATY", {1, 0, 57});
    typeless.insert(typeless.end(), unknownType.begin(), unknownType.end());
    const std::string bytes = made_metallib::thin(
        {"flagged", "high_bit", "unknown_type"}, {2, 0, 0},
        {made_metallib::wrappedBitcode(0x31), made_metallib::wrappedBitcode(0x32), made_metallib::wrappedBitcode(0x33)},
        {flagTwo, highBit, typeless});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_EQ(constants([library newFunctionWithName:@"flagged"]), "") << "a flag byte of 2";
    EXPECT_EQ(attributes([library newFunctionWithName:@"high_bit"].vertexAttributes), "nil") << "VATT bit 12";
    EXPECT_EQ(attributes([library newFunctionWithName:@"unknown_type"].stageInputAttributes), "nil")
        << "MTLDataType 57";
    expectNoM2V(device);
}

}  // namespace

TEST(RawFacadeFunctionTags, AnswersAreMetalsForAMetalBuiltLibrary)
{
    switch_ios::test::ExpectPassesInFreshProcess(measuredAnswers);
}

TEST(RawFacadeFunctionTags, UnmeasuredTagsAnswerAsM2VDid)
{
    switch_ios::test::ExpectPassesInFreshProcess(unmeasuredTags);
}
