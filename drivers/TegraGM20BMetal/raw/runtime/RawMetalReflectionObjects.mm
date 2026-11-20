// SPDX-License-Identifier: MIT
// See RawMetalReflection.h: a blob's nodes as objects of one class per kind, as Metal's are. A binding answers
// MTLArgument's selectors and each type class its own, so a selector Metal's class lacks is unrecognized here too. A
// binding asked another binding type's selector (a texture's bufferAlignment) answers 0, NO or nil with one error line:
// Metal fails an assertion there, which aborts the process and no caller can catch.
#import "RawMetalReflection.h"

#import <objc/runtime.h>
#include <iterator>
#include "RawMetalInternal.h"
#include "raw_reflection_blob.h"

@interface RMReflectionNode : NSObject {
@public
    raw::ReflectionNode _node;
    NSArray *_members;
    id _children[4];
    NSString *_name;
}
@end

@implementation RMReflectionNode

- (BOOL)isKindOfClass:(Class)cls
{
    static const Class reflected[] = {Nil,
                                      [MTLArgument class],
                                      [MTLStructType class],
                                      [MTLStructMember class],
                                      [MTLArrayType class],
                                      [MTLPointerType class],
                                      [MTLTextureReferenceType class]};
    const Class kind = _node.kind < std::size(reflected) ? reflected[_node.kind] : Nil;
    return (kind && [kind isSubclassOfClass:cls]) || [super isKindOfClass:cls];
}

@end

@interface RMReflectionBinding : RMReflectionNode
@end

static bool asked(RMReflectionBinding *b, MTLBindingType type, SEL selector)
{
    if (b->_node.values[0] == uint64_t(type))
        return true;
    os_log_error(rawLog(), "raw: %{public}s: %{public}s asked of binding %{public}s of type %llu, where Metal asserts",
                 getprogname(), sel_getName(selector), b->_name.UTF8String ?: "-",
                 (unsigned long long)b->_node.values[0]);
    return false;
}

@implementation RMReflectionBinding

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    Protocol *binding = @protocol(MTLBinding);
    switch (_node.values[0]) {
    case MTLBindingTypeBuffer:
        binding = @protocol(MTLBufferBinding);
        break;
    case MTLBindingTypeTexture:
        binding = @protocol(MTLTextureBinding);
        break;
    case MTLBindingTypeThreadgroupMemory:
        binding = @protocol(MTLThreadgroupBinding);
        break;
    }
    return protocol_conformsToProtocol(binding, protocol) || [super conformsToProtocol:protocol];
}

- (NSString *)name
{
    return _name;
}

- (NSUInteger)type
{
    return _node.values[0];
}

- (NSUInteger)access
{
    return _node.values[1];
}

- (NSUInteger)index
{
    return _node.values[2];
}

- (BOOL)isUsed
{
    return _node.flags & 1;
}

- (BOOL)isActive
{
    return [self isUsed];
}

- (BOOL)isArgument
{
    return _node.flags & 2;
}

- (NSUInteger)arrayLength
{
    return _node.values[7];
}

- (NSUInteger)bufferAlignment
{
    return asked(self, MTLBindingTypeBuffer, _cmd) ? _node.values[5] : 0;
}

- (NSUInteger)bufferDataSize
{
    return asked(self, MTLBindingTypeBuffer, _cmd) ? _node.values[6] : 0;
}

- (MTLDataType)bufferDataType
{
    return asked(self, MTLBindingTypeBuffer, _cmd) ? MTLDataType(_node.values[3]) : MTLDataTypeNone;
}

- (id)bufferStructType
{
    return asked(self, MTLBindingTypeBuffer, _cmd) ? _children[0] : nil;
}

- (id)bufferPointerType
{
    return _children[3];
}

- (id)structType
{
    return _children[0];
}

- (NSUInteger)threadgroupMemoryAlignment
{
    return asked(self, MTLBindingTypeThreadgroupMemory, _cmd) ? _node.values[5] : 0;
}

- (NSUInteger)threadgroupMemoryDataSize
{
    return asked(self, MTLBindingTypeThreadgroupMemory, _cmd) ? _node.values[6] : 0;
}

- (MTLTextureType)textureType
{
    return asked(self, MTLBindingTypeTexture, _cmd) ? MTLTextureType(_node.values[5]) : MTLTextureType1D;
}

- (MTLDataType)textureDataType
{
    return asked(self, MTLBindingTypeTexture, _cmd) ? MTLDataType(_node.values[6]) : MTLDataTypeNone;
}

- (BOOL)isDepthTexture
{
    return asked(self, MTLBindingTypeTexture, _cmd) && (_node.flags & 4);
}

@end

@interface RMReflectionStructType : RMReflectionNode
@end

@implementation RMReflectionStructType

- (MTLDataType)dataType
{
    return MTLDataTypeStruct;
}

- (NSArray *)members
{
    return _members;
}

// The last of the members of that name, as Metal answers among anonymous members (several named "")
- (id)memberByName:(NSString *)name
{
    for (RMReflectionNode *member in [_members reverseObjectEnumerator])
        if ([member->_name isEqualToString:name])
            return member;
    return nil;
}

@end

@interface RMReflectionMember : RMReflectionNode
@end

@implementation RMReflectionMember

- (NSString *)name
{
    return _name;
}

- (MTLDataType)dataType
{
    return MTLDataType(_node.values[0]);
}

- (NSUInteger)offset
{
    return _node.values[1];
}

- (NSUInteger)argumentIndex
{
    return _node.values[2];
}

- (id)structType
{
    return _children[0];
}

- (id)arrayType
{
    return _children[1];
}

- (id)textureReferenceType
{
    return _children[2];
}

- (id)pointerType
{
    return _children[3];
}

@end

@interface RMReflectionArrayType : RMReflectionNode
@end

@implementation RMReflectionArrayType

- (MTLDataType)dataType
{
    return MTLDataTypeArray;
}

- (MTLDataType)elementType
{
    return MTLDataType(_node.values[0]);
}

- (NSUInteger)arrayLength
{
    return _node.values[1];
}

- (NSUInteger)stride
{
    return _node.values[2];
}

- (NSUInteger)argumentIndexStride
{
    return _node.values[3];
}

- (id)elementStructType
{
    return _children[0];
}

- (id)elementArrayType
{
    return _children[1];
}

- (id)elementTextureReferenceType
{
    return _children[2];
}

- (id)elementPointerType
{
    return _children[3];
}

@end

@interface RMReflectionPointerType : RMReflectionNode
@end

@implementation RMReflectionPointerType

- (MTLDataType)dataType
{
    return MTLDataTypePointer;
}

- (MTLDataType)elementType
{
    return MTLDataType(_node.values[0]);
}

- (NSUInteger)access
{
    return _node.values[1];
}

- (NSUInteger)alignment
{
    return _node.values[2];
}

- (NSUInteger)dataSize
{
    return _node.values[3];
}

- (BOOL)elementIsArgumentBuffer
{
    return _node.flags & 1;
}

- (id)elementStructType
{
    return _children[0];
}

- (id)elementArrayType
{
    return _children[1];
}

- (id)structType
{
    return _children[0];
}

@end

@interface RMReflectionTextureType : RMReflectionNode
@end

@implementation RMReflectionTextureType

- (MTLDataType)dataType
{
    return MTLDataTypeTexture;
}

- (MTLDataType)textureDataType
{
    return MTLDataType(_node.values[0]);
}

- (MTLTextureType)textureType
{
    return MTLTextureType(_node.values[1]);
}

- (NSUInteger)access
{
    return _node.values[2];
}

- (BOOL)isDepthTexture
{
    return _node.flags & 1;
}

@end

@interface RMComputeReflection : NSObject
@property (nonatomic, copy) NSArray *bindings;
@end

@implementation RMComputeReflection

- (BOOL)isKindOfClass:(Class)cls
{
    return [[MTLComputePipelineReflection class] isSubclassOfClass:cls] || [super isKindOfClass:cls];
}

- (NSArray *)arguments
{
    return _bindings;
}

@end

@interface RMRenderReflection : NSObject
@property (nonatomic, copy) NSArray *vertexBindings, *fragmentBindings, *tileBindings, *objectBindings, *meshBindings;
@end

@implementation RMRenderReflection

- (BOOL)isKindOfClass:(Class)cls
{
    return [[MTLRenderPipelineReflection class] isSubclassOfClass:cls] || [super isKindOfClass:cls];
}

- (NSArray *)vertexArguments
{
    return _vertexBindings;
}

- (NSArray *)fragmentArguments
{
    return _fragmentBindings;
}

- (NSArray *)tileArguments
{
    return _tileBindings;
}

@end

id rmReflectionOf(const uint8_t *bytes, size_t length, bool compute)
{
    raw::ReflectionView v;
    if (!raw::readReflection(bytes, length, compute ? raw::kReflectionComputeStages : raw::kReflectionRenderStages, v))
        return nil;
    static const Class kinds[] = {Nil,
                                  [RMReflectionBinding class],
                                  [RMReflectionStructType class],
                                  [RMReflectionMember class],
                                  [RMReflectionArrayType class],
                                  [RMReflectionPointerType class],
                                  [RMReflectionTextureType class]};
    NSMutableArray<RMReflectionNode *> *objects = [NSMutableArray arrayWithCapacity:v.header->nodes];
    for (uint32_t i = 0; i < v.header->nodes; i++) {
        RMReflectionNode *value = [kinds[v.nodes[i].kind] new];
        value->_node = v.nodes[i];
        const char *name = v.name(v.nodes[i]);
        value->_name = name ? @(name) : nil;
        for (uint32_t c = 0; c < 4; c++)
            if (v.nodes[i].children[c] != raw::kReflectionAbsent)
                value->_children[c] = objects[v.nodes[i].children[c]];
        NSMutableArray *members = [NSMutableArray arrayWithCapacity:v.nodes[i].count];
        for (uint32_t e = 0; e < v.nodes[i].count; e++)
            [members addObject:objects[v.edges[v.nodes[i].first + e]]];
        value->_members = members;
        [objects addObject:value];
    }
    NSMutableArray<NSArray *> *stages = [NSMutableArray arrayWithCapacity:v.header->stages];
    for (uint32_t s = 0; s < v.header->stages; s++) {
        NSMutableArray *bindings = [NSMutableArray arrayWithCapacity:v.stages[s].count];
        for (uint32_t e = 0; e < v.stages[s].count; e++)
            [bindings addObject:objects[v.edges[v.stages[s].first + e]]];
        [stages addObject:bindings];
    }
    if (compute) {
        RMComputeReflection *r = [RMComputeReflection new];
        r.bindings = stages[0];
        return r;
    }
    RMRenderReflection *r = [RMRenderReflection new];
    r.vertexBindings = stages[0];
    r.fragmentBindings = stages[1];
    r.tileBindings = stages[2];
    r.objectBindings = stages[3];
    r.meshBindings = stages[4];
    return r;
}
