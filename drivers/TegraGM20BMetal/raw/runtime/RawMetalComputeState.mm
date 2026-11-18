// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalBinaryArchive.h"
#import "RawMetalComputeState.h"
#import "RawMetalReflection.h"
#include "RawComplete.h"
#include "RawCompileRequest.h"
#include "raw_compute_key.h"
#include "raw_metallib.h"
#include "raw_pack.h"

@implementation RMComputeState

+ (Class)twinClass
{
    return NSClassFromString(@"NBComputePipeline");
}

- (Class)rmTwinClass
{
    return [[self class] twinClass];
}

- (id)rmTwin
{
    std::call_once(_twinOnce, [self] {
        if (!_compatibility) {
            NSError *error = nil;
            _compatibility =
                [RM_M2V(NeedComputeStateTwin) newComputePipelineStateWithDescriptor:twinComputeDescriptor(_direct)
                                                                            options:MTLPipelineOptionNone
                                                                         reflection:nil
                                                                              error:&error];
            if (!_compatibility)
                once([NSString stringWithFormat:@"a deferred compute pipeline could not be created: %@", error]);
        }
    });
    return _compatibility;
}

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    return protocol == @protocol(MTLComputePipelineState) || [super conformsToProtocol:protocol];
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return _facts.maxThreads;
}

- (NSUInteger)threadExecutionWidth
{
    return _facts.executionWidth;
}

- (NSUInteger)staticThreadgroupMemoryLength
{
    return _facts.staticMemory;
}

// The descriptor's, as Metal's state answers it, without making m2v's twin
- (BOOL)supportIndirectCommandBuffers
{
    return _direct.supportIndirectCommandBuffers;
}

@end

MTLComputePipelineDescriptor *twinComputeDescriptor(MTLComputePipelineDescriptor *d)
{
    id<MTLFunction> twin = twinOf(d.computeFunction);
    if (!twin)
        return nil;
    MTLComputePipelineDescriptor *t = [d copy];
    t.binaryArchives = nil;  // this device's (RawMetalBinaryArchive.mm), which m2v cannot read
    t.computeFunction = twin;
    return t;
}

// The function's AIR hash and its library function's name, as the facade's library read them
static bool airOf(id<MTLFunction> function, raw::AirHash &out, NSString *__strong &name)
{
    if (![(id)function isKindOfClass:[RMFunction class]] || !((RMFunction *)function)->_air)
        return false;
    RMFunction *f = (RMFunction *)function;
    out = *f->_air;
    name = @(libraryName(f).c_str());
    return true;
}

// The descriptor's linked functions as the compute key takes them (raw_compute_key.h computeLinkedText); false for any the
// key cannot name: another device's
static bool linkedFunctions(MTLComputePipelineDescriptor *d, std::vector<raw::LinkedComputeFunction> &linked)
{
    MTLLinkedFunctions *l = d.linkedFunctions;
    const auto add = [&](id function, uint32_t list, std::optional<std::string> group = std::nullopt) {
        raw::LinkedComputeFunction f;
        f.hidden = list == 1;
        f.binary = list == 2;
        f.group = std::move(group);
        NSString *libraryName = nil;
        if (![function isKindOfClass:[RMFunction class]] || !airOf(function, f.air, libraryName))
            return false;
        f.constants = ((RMFunction *)function)->_constants;
        f.symbol = libraryName.UTF8String ?: "";
        f.name = [function name].UTF8String ?: "";
        linked.push_back(std::move(f));
        return true;
    };
    for (const uint32_t list : {0u, 1u, 2u})
        for (id function in list == 2 ? l.binaryFunctions : list == 1 ? l.privateFunctions : l.functions)
            if (!add(function, list))
                return false;
    for (NSString *group in [l.groups.allKeys sortedArrayUsingSelector:@selector(compare:)])
        for (id function in l.groups[group])
            if (!add(function, 3,
                     std::string(group.UTF8String ?: "", [group lengthOfBytesUsingEncoding:NSUTF8StringEncoding])))
                return false;
    return true;
}

// The creations under MTLPipelineOptionFailOnBinaryArchiveMiss lists (this device's, RawMetalBinaryArchive.mm), as
// Metal's answer: false (and Metal's error) where none holds the state and the pack holds no kernel of it, never compiled.
// An archive names a state by its function and `constants` alone, never its linked functions.
static bool archivesAnswer(RMComputeState *s, RMFunction *f, const std::string &constants, MTLPipelineOption &options,
                           NSError **error)
{
    NSString *name = s->_libraryName ?: f.name;
    const std::string key =
        raw::computeArchiveKey(name.UTF8String ?: "", constants, s->_maxThreads, s->_multipleOfWidth);
    const raw::ArchiveAnswer answer =
        rmArchiveAnswer(s->_direct.binaryArchives, options, raw::Pack::fnv(key.data(), key.size()));
    options &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    s->_direct.binaryArchives = nil;
    if (answer != raw::ArchiveAnswer::Missed ||
        rmPackHoldsKernels(raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth)))
        return true;
    if (error)
        *error = rmArchiveMissError(f.name, "compute");
    return false;
}

// The answers of a keyed state the pack has no facts of, the shader compiler's (RawComplete.h facts); a stage input is keyed
// after the constants and sent beside them (KernelRequest::stageIn)
static bool compilerAnswers(RMComputeState *s, RMFunction *f, NSError **error)
{
    std::string failure = "no shader compiler comes this boot";
    raw::PackComputeFacts made = {};
    const std::string key = raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth);
    if (!gRM->completion || !gRM->completion->facts(f, s->_direct, key, made, failure)) {
        if (error)
            *error = rmCreationError(failure);
        return false;
    }
    s->_facts = made;
    if (error)
        *error = nil;
    return true;
}

id rmComputeState(id device, MTLComputePipelineDescriptor *descriptor, MTLPipelineOption options,
                  MTLAutoreleasedComputePipelineReflection *reflection, NSError **error)
{
    descriptor = [descriptor copy];
    for (id<MTLFunction> binary in descriptor.linkedFunctions.binaryFunctions)
        if (![(id)binary isKindOfClass:[RMFunction class]] || !(binary.options & MTLFunctionOptionCompileToBinary)) {
            if (error)
                *error = rmCreationError(
                    "a binary function must be this device's visible function made with CompileToBinary");
            return nil;
        }
    id<MTLFunction> function = descriptor.computeFunction;
    RMComputeState *s = [RMComputeState new];
    s->_device = device;
    RMFunction *f = [(id)function isKindOfClass:[RMFunction class]] ? (RMFunction *)function : nil;
    NSString *libraryName = nil;
    std::vector<raw::LinkedComputeFunction> linked;
    // a function of another device (Metal's validation error), or one whose library lists no AIR hash of it, which no
    // compile request can name
    if (!f || !linkedFunctions(descriptor, linked) || !airOf(function, s->_air, libraryName)) {
        constexpr const char *unkeyed =
            "a compute or linked function of another device, or one whose library lists no AIR hash of it";
        os_log_error(rawLog(), "raw: %{public}s: no compute state: %{public}s (%{public}s)", getprogname(), unkeyed,
                     descriptor.label.UTF8String ?: "-");
        if (error)
            *error = rmCreationError(unkeyed);
        return nil;
    }
    s->_libraryName = libraryName;
    const std::string stageIn = raw::stageInText(raw::stageInFieldsOf(descriptor.stageInputDescriptor));
    s->_stageIn = !stageIn.empty();
    s->_constants = raw::computeStateConstants(f->_constants, stageIn, linked);
    s->_maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
    s->_multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    s->_direct = descriptor;
    s->_function = f;
    if (!archivesAnswer(s, f, f->_constants + stageIn, options, error))
        return nil;
    if (const raw::PackComputeFacts *facts =
            rmPackComputeFacts(raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth))) {
        s->_facts = *facts;
        if (error)
            *error = nil;
    } else if (!compilerAnswers(s, f, error)) {
        return nil;
    }
    if (reflection && !rmComputeReflection(s, options, reflection, error))
        return nil;
    s->_threadLimit = uint32_t(std::min<NSUInteger>(1024, s.maxTotalThreadsPerThreadgroup));
    if (s->_maxThreads)
        s->_threadLimit = std::min(s->_threadLimit, s->_maxThreads);
    s.label = descriptor.label;
    return s;
}
