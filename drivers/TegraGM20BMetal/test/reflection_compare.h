// SPDX-License-Identifier: MIT
// Two reflection objects compared selector by selector, recursively: every answer Metal's reflection classes give
// (MTLBinding and its kinds, MTLStructType, MTLStructMember, MTLArrayType, MTLPointerType, MTLTextureReferenceType),
// Metal's against the raw facade's.
#pragma once
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace reflection_compare {

// What Metal answers each kind of object: it fails an assertion on a binding asked another type's selector (a texture
// binding's bufferAlignment) and does not implement a type class's selectors on another type class. Probed on macOS 27.2
// per binding type and type class; the first object's kind picks the set, and raw is asked the same set.
struct Selectors {
    std::vector<const char *> counts, flags, objects;
    bool named = false, members = false;
};

inline Selectors selectorsOf(id metal)
{
    if ([metal conformsToProtocol:@protocol(MTLBinding)]) {
        Selectors s{{"type", "access", "index", "arrayLength"},
                    {"isUsed", "isActive", "isArgument"},
                    {"structType", "bufferPointerType"},
                    true};
        switch (((NSUInteger (*)(id, SEL))objc_msgSend)(metal, @selector(type))) {
        case MTLBindingTypeBuffer:
            s.counts.insert(s.counts.end(), {"bufferAlignment", "bufferDataSize", "bufferDataType"});
            s.objects.push_back("bufferStructType");
            break;
        case MTLBindingTypeTexture:
            s.counts.insert(s.counts.end(), {"textureType", "textureDataType"});
            s.flags.push_back("isDepthTexture");
            break;
        case MTLBindingTypeThreadgroupMemory:
            s.counts.insert(s.counts.end(), {"threadgroupMemoryAlignment", "threadgroupMemoryDataSize"});
            break;
        default:
            break;
        }
        return s;
    }
    if ([metal isKindOfClass:[MTLStructMember class]])
        return {{"offset", "dataType", "argumentIndex"},
                {},
                {"structType", "arrayType", "textureReferenceType", "pointerType"},
                true};
    if ([metal isKindOfClass:[MTLStructType class]])
        return {{"dataType"}, {}, {}, false, true};
    if ([metal isKindOfClass:[MTLArrayType class]])
        return {{"dataType", "elementType", "arrayLength", "stride", "argumentIndexStride"},
                {},
                {"elementStructType", "elementArrayType", "elementTextureReferenceType", "elementPointerType"}};
    if ([metal isKindOfClass:[MTLPointerType class]])
        return {{"dataType", "elementType", "access", "alignment", "dataSize"},
                {"elementIsArgumentBuffer"},
                {"elementStructType", "elementArrayType", "structType"}};
    if ([metal isKindOfClass:[MTLTextureReferenceType class]])
        return {{"dataType", "textureType", "textureDataType", "access"}, {"isDepthTexture"}, {}};
    return {};
}

inline void expectSame(id metal, id raw, const std::string &path);

inline void expectSameList(NSArray *metal, NSArray *raw, const std::string &path)
{
    ASSERT_EQ(metal.count, raw.count) << path;
    for (NSUInteger i = 0; i < metal.count; i++)
        expectSame(metal[i], raw[i], path + "[" + std::to_string(i) + "]");
}

inline void expectSame(id metal, id raw, const std::string &path)
{
    ASSERT_EQ(metal == nil, raw == nil) << path;
    if (!metal)
        return;
    for (Class c : {[MTLArgument class],
                    [MTLStructType class],
                    [MTLStructMember class],
                    [MTLArrayType class],
                    [MTLPointerType class],
                    [MTLTextureReferenceType class],
                    [MTLType class]})
        EXPECT_EQ([metal isKindOfClass:c], [raw isKindOfClass:c]) << path << " " << class_getName(c);
    for (Protocol *p : {
             @protocol (MTLBinding), @protocol (MTLBufferBinding), @protocol (MTLTextureBinding),
                 @protocol (MTLThreadgroupBinding)
         })
        EXPECT_EQ([metal conformsToProtocol:p], [raw conformsToProtocol:p]) << path << " " << protocol_getName(p);
    const Selectors selectors = selectorsOf(metal);
    if (selectors.named)
        EXPECT_TRUE([[metal name] isEqual:[raw name]] || (![metal name] && ![raw name])) << path;
    for (const char *s : selectors.counts)
        EXPECT_EQ(((NSUInteger (*)(id, SEL))objc_msgSend)(metal, sel_registerName(s)),
                  ((NSUInteger (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)))
            << path << " " << s;
    for (const char *s : selectors.flags)
        EXPECT_EQ(((BOOL (*)(id, SEL))objc_msgSend)(metal, sel_registerName(s)),
                  ((BOOL (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)))
            << path << " " << s;
    for (const char *s : selectors.objects)
        expectSame(((id (*)(id, SEL))objc_msgSend)(metal, sel_registerName(s)),
                   ((id (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)), path + "." + s);
    if (!selectors.members)
        return;
    expectSameList([metal members], [raw members], path + ".members");
    for (id member in [metal members])
        expectSame([metal memberByName:[member name]], [raw memberByName:[member name]], path + ".memberByName");
}

}  // namespace reflection_compare
