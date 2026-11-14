// SPDX-License-Identifier: MIT
#import "RawMetalFunctionTags.h"
#import "RawMetalInternal.h"
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

bool rmFunctionAttributes(RMFunction *f, bool stageInput, NSArray **out)
{
    *out = nil;
    raw::FunctionTags tags;
    if (!tagsOf(f, tags))
        return true;
    if (!tags.attributes) {
        os_log_error(rawLog(),
                     "raw: %{public}s: function %{public}s's attributes are of a kind Metal's answer was not "
                     "measured for: nil given",
                     getprogname(), f->_libraryName.c_str());
        return true;
    }
    if (tags.attributes->empty() || (!stageInput && f->_type != MTLFunctionTypeVertex))
        return true;
    if (f->_specialized)
        return false;
    NSMutableArray *attributes = [NSMutableArray arrayWithCapacity:tags.attributes->size()];
    for (const raw::FunctionAttributeTag &a : *tags.attributes) {
        RMAttribute *attribute = [RMAttribute new];
        attribute.name = @(a.name.c_str());
        attribute.attributeIndex = a.index;
        attribute.attributeType = MTLDataType(a.type);
        attribute.active = a.active;
        attribute.patchData = a.patchData;
        attribute.patchControlPointData = a.controlPointData;
        [attributes addObject:attribute];
    }
    *out = attributes;
    return true;
}
