// SPDX-License-Identifier: MIT
#import "RawMetalFunctionTags.h"
#import "RawMetalInternal.h"
#include "RawComplete.h"
#include "raw_function_attributes.h"
#include "raw_function_tags.h"

// MTLFunctionConstant's readable properties (Metal's own class has no public initializer)
@interface RMFunctionConstant : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) MTLDataType type;
@property (nonatomic) NSUInteger index;
@property (nonatomic) BOOL required;
@end
@implementation RMFunctionConstant
@end

// MTLVertexAttribute's and MTLAttribute's readable properties
@interface RMAttribute : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) NSUInteger attributeIndex;
@property (nonatomic) MTLDataType attributeType;
@property (nonatomic, getter=isActive) BOOL active;
@property (nonatomic, getter=isPatchData) BOOL patchData;
@property (nonatomic, getter=isPatchControlPointData) BOOL patchControlPointData;
@end
@implementation RMAttribute
@end

static bool tagsOf(RMFunction *f, raw::FunctionTags &tags)
{
    if (f->_source && f->_source->bytes &&
        raw::functionTags(f->_source->bytes, f->_source->size, f->_libraryName, tags))
        return true;
    os_log_error(rawLog(), "raw: %{public}s: no public metadata of function %{public}s in its library", getprogname(),
                 f->_libraryName.c_str());
    return false;
}

NSDictionary<NSString *, MTLFunctionConstant *> *rmFunctionConstants(RMFunction *f)
{
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    raw::FunctionTags tags;
    if (f->_specialized || !tagsOf(f, tags))
        return (NSDictionary<NSString *, MTLFunctionConstant *> *)out;
    if (!tags.constants)
        os_log_error(rawLog(),
                     "raw: %{public}s: function %{public}s lists a constant of a type or flag Metal's answer was "
                     "not measured for: none given",
                     getprogname(), f->_libraryName.c_str());
    for (const raw::FunctionConstantTag &c : tags.constants.value_or(std::vector<raw::FunctionConstantTag>{})) {
        RMFunctionConstant *constant = [RMFunctionConstant new];
        constant.name = @(c.name.c_str());
        constant.type = MTLDataType(c.type);
        constant.index = c.index;
        constant.required = c.required;
        out[constant.name] = constant;
    }
    return (NSDictionary<NSString *, MTLFunctionConstant *> *)out;
}

static NSArray *attributeObjects(const std::vector<raw::FunctionAttributeTag> &list)
{
    NSMutableArray *attributes = [NSMutableArray arrayWithCapacity:list.size()];
    for (const raw::FunctionAttributeTag &a : list) {
        RMAttribute *attribute = [RMAttribute new];
        attribute.name = @(a.name.c_str());
        attribute.attributeIndex = a.index;
        attribute.attributeType = MTLDataType(a.type);
        attribute.active = a.active;
        attribute.patchData = a.patchData;
        attribute.patchControlPointData = a.controlPointData;
        [attributes addObject:attribute];
    }
    return attributes;
}

// A function made with constant values that has attributes: the shader compiler's answer of its specialized AIR
static NSArray *specializedAttributes(RMFunction *f, bool stageInput)
{
    const void *bytes = nullptr;
    size_t length = 0;
    NSString *libraryFunction = nil;
    std::map<std::string, raw::AirHash> hashes;
    std::string blob, why = "the function's library has no AIR of it";
    raw::AttributeList vertex, stage;
    if (rmFunctionSource((id<MTLFunction>)f, &bytes, &length, &libraryFunction) &&
        raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes) &&
        hashes.count(libraryFunction.UTF8String ?: "")) {
        const std::string key =
            raw::functionAttributesKey(hashes[libraryFunction.UTF8String], f->_constants, f.name.UTF8String ?: "");
        if (!gRM->completion)
            why = "no shader compiler comes this boot";
        else if (gRM->completion->attributes(f, key, blob, why) && raw::decodeAttributeLists(blob, vertex, stage)) {
            const raw::AttributeList &list = stageInput ? stage : vertex;
            return list ? attributeObjects(*list) : nil;
        }
    }
    os_log_error(rawLog(), "raw: %{public}s: no attributes of specialized function %{public}s: %{public}s, nil given",
                 getprogname(), f.name.UTF8String ?: "?", why.c_str());
    return nil;
}

NSArray *rmFunctionAttributes(RMFunction *f, bool stageInput)
{
    raw::FunctionTags tags;
    if (!tagsOf(f, tags))
        return nil;
    if (!tags.attributes) {
        os_log_error(rawLog(),
                     "raw: %{public}s: function %{public}s's attributes are of a kind Metal's answer was not "
                     "measured for: nil given",
                     getprogname(), f->_libraryName.c_str());
        return nil;
    }
    if (tags.attributes->empty() || (!stageInput && f->_type != MTLFunctionTypeVertex))
        return nil;
    return f->_specialized ? specializedAttributes(f, stageInput) : attributeObjects(*tags.attributes);
}
