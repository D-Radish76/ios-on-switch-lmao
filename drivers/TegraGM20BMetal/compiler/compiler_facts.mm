// SPDX-License-Identifier: MIT
// See compiler_facts.h.
#import "compiler_facts.h"

#import <Foundation/Foundation.h>
#include <vector>
#include "MetallibSlice.h"
#include "TegraGM20BCompilerABI.h"
#include "bridge/NativeMetalDirect.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/RawPipelineKey.h"
#include "../raw/runtime/raw_buffer_encoders.h"
#include "../raw/runtime/raw_compute_facts_entry.h"
#include "../raw/runtime/raw_function_attributes.h"
#include "../raw/runtime/raw_reflection_blob.h"

namespace switch_compiler {
namespace {

// The AIR slice of the library `l` names: a system library's file under `root` (`held` keeps its mapping), else its bytes
bool airSliceOf(const raw::RequestLibrary &l, const std::string &root, NSData *__strong &held, const uint8_t *&slice,
                size_t &size, std::string &why)
{
    const uint8_t *file = reinterpret_cast<const uint8_t *>(l.bytes.data());
    size_t length = l.bytes.size();
    if (l.system) {
        std::string real;
        if (!SystemLibraries::resolve(root, l.path, real, why))
            return false;
        held = [NSData dataWithContentsOfFile:@(real.c_str()) options:NSDataReadingMappedIfSafe error:nil];
        if (!held)
            return why = "an unreadable library: " + l.path, false;
        file = static_cast<const uint8_t *>(held.bytes);
        length = held.length;
    }
    size_t at = 0;
    if (!switch_metal::airSlice(file, length, at, size))
        return why = "no AIR slice Metal takes in " + (l.path.empty() ? std::string("its library's data") : l.path),
               false;
    slice = file + at;
    return true;
}

// The constants as the translator takes them, indexed then named (raw/pack/offline_metal.mm)
bool constantsOf(MTLFunctionConstantValues *values, std::vector<NmbConstant> &out, std::string &why)
{
    NSArray *indexed = [values newIndexedConstantArray], *named = [values newNamedConstantArray];
    for (unsigned group = 0; group < 2; group++)
        for (id<RMConstantValue> v in (group ? named : indexed)) {
            const uint64_t bytes = raw::constantBytes(v.dataType);
            if (!bytes || !v.data)
                return why = "a constant of type " + std::to_string(uint64_t(v.dataType)) + " it cannot pass", false;
            out.push_back({v.dataType, group ? 0 : v.index, group ? v.name.UTF8String : nullptr, v.data, bytes});
        }
    return true;
}

NmbOfflineFunction *functionOf(const NmbOfflineLibrary *library, const raw::RequestStage &s,
                               std::vector<NmbConstant> &constants, MTLFunctionConstantValues *__strong &values,
                               std::string &why)
{
    if (!s.specialized) {
        if (!s.constants.empty())
            return why = "constants of a function made without them", nullptr;
        return nmb_offline_function_new(library, s.function.c_str());
    }
    if (!(values = raw::constantValuesOf(s.constants)))
        return why = "constants it cannot read: " + s.constants, nullptr;
    if (!constantsOf(values, constants, why))
        return nullptr;
    return nmb_offline_function_new_specialized(library, s.function.c_str(),
                                                s.name != s.function ? s.name.c_str() : nullptr, constants.data(),
                                                uint32_t(constants.size()));
}

// vertexAttributes (`stageInput` false) or stageInputAttributes as the translator answers them; nullopt for nil
raw::AttributeList attributesOf(const NmbOfflineFunction *f, bool stageInput)
{
    const int64_t count = stageInput ? nmb_offline_function_stage_input_attribute_count(f)
                                     : nmb_offline_function_vertex_attribute_count(f);
    if (count < 0)
        return std::nullopt;
    std::vector<raw::FunctionAttributeTag> out;
    for (uint32_t i = 0; i < uint32_t(count); i++) {
        char name[256];
        uint64_t info[5] = {};
        const size_t named = stageInput ? nmb_offline_function_stage_input_attribute(f, i, name, sizeof name, info)
                                        : nmb_offline_function_vertex_attribute(f, i, name, sizeof name, info);
        if (!named)
            return std::nullopt;
        raw::FunctionAttributeTag a;
        a.name = name;
        a.index = uint8_t(info[0]);
        a.type = uint8_t(info[1]);
        a.active = info[2];
        a.patchData = info[3];
        a.controlPointData = info[4];
        out.push_back(std::move(a));
    }
    return out;
}

bool collectEncoder(uint32_t index, bool argumentBuffer, const NmbArgumentLayout *layouts, uint32_t count,
                    const char *refusal, void *ctx)
{
    static_assert(sizeof(raw::ArgumentRun) == sizeof(NmbArgumentRun), "the bridge's runs");
    raw::BufferEncoder e;
    e.index = index;
    e.argumentBuffer = argumentBuffer;
    e.refusal = refusal ? refusal : "";
    for (uint32_t i = 0; i < count; i++) {
        const auto *runs = reinterpret_cast<const raw::ArgumentRun *>(layouts[i].runs);
        e.layouts.push_back({layouts[i].encoded_length, layouts[i].alignment,
                             std::vector<raw::ArgumentRun>(runs, runs + layouts[i].run_count)});
    }
    static_cast<raw::BufferEncoders *>(ctx)->push_back(std::move(e));
    return true;
}

// `s`'s function with the request's constants, made by the translator alone; null with `why`, `library` released
NmbOfflineFunction *requestFunction(const raw::RequestStage &s, const raw::RequestLibrary &l, const std::string &root,
                                    NmbOfflineLibrary *&library, std::string &why)
{
    NSData *held = nil;
    const uint8_t *slice = nullptr;
    size_t size = 0;
    if (!airSliceOf(l, root, held, slice, size, why))
        return nullptr;
    if (!(library = nmb_offline_library_new(slice, size)))
        return why = std::string("a library the translator does not read: ") + nmb_last_error(), nullptr;
    std::vector<NmbConstant> constants;
    MTLFunctionConstantValues *values = nil;
    NmbOfflineFunction *function = functionOf(library, s, constants, values, why);
    if (function)
        return function;
    if (why.empty())
        why = "no function " + s.function + ": " + nmb_last_error();
    nmb_offline_library_release(library);
    library = nullptr;
    return nullptr;
}

}  // namespace

int32_t makeBufferEncoders(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why)
{
    @autoreleasepool {
        const raw::RequestStage &s = p.request.stages[0];
        NmbOfflineLibrary *library = nullptr;
        NmbOfflineFunction *function = requestFunction(s, p.request.libraries[s.library], systemRoot, library, why);
        if (!function)
            return TegraGM20BCompileUnavailable;
        raw::BufferEncoders encoders;
        const bool made = nmb_offline_function_buffer_encoders(function, collectEncoder, &encoders);
        if (!made)
            why = "the translator does not decode " + s.function + ": " + nmb_last_error();
        nmb_offline_function_release(function);
        nmb_offline_library_release(library);
        const std::string blob = raw::encodeBufferEncoders(encoders);
        entry = raw::encodeReflectionEntry(p.key, made ? &blob : nullptr, why);
        return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
    }
}

int32_t makeAttributes(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why)
{
    @autoreleasepool {
        const raw::RequestStage &s = p.request.stages[0];
        NmbOfflineLibrary *library = nullptr;
        NmbOfflineFunction *function = requestFunction(s, p.request.libraries[s.library], systemRoot, library, why);
        if (!function)
            return TegraGM20BCompileUnavailable;
        const std::string blob = raw::encodeAttributeLists(attributesOf(function, false), attributesOf(function, true));
        nmb_offline_function_release(function);
        nmb_offline_library_release(library);
        entry = raw::encodeReflectionEntry(p.key, &blob, why);
        return TegraGM20BCompileMade;
    }
}

int32_t makeFacts(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why)
{
    @autoreleasepool {
        const raw::RequestStage &s = p.request.stages[0];
        NSData *held = nil;
        const uint8_t *slice = nullptr;
        size_t size = 0;
        if (!airSliceOf(p.request.libraries[s.library], systemRoot, held, slice, size, why))
            return TegraGM20BCompileUnavailable;
        NmbOfflineLibrary *library = nmb_offline_library_new(slice, size);
        if (!library)
            return why = std::string("a library the translator does not read: ") + nmb_last_error(),
                   TegraGM20BCompileUnavailable;
        std::vector<NmbConstant> constants;
        MTLFunctionConstantValues *values = nil;
        NmbOfflineFunction *function = functionOf(library, s, constants, values, why);
        if (!function) {
            if (why.empty())
                why = "no function " + s.function + ": " + nmb_last_error();
            nmb_offline_library_release(library);
            return TegraGM20BCompileUnavailable;
        }
        NmbOfflineComputePipelineDescriptor d = {};
        d.function = function;
        d.max_threads = p.request.kernel.maxThreads;
        d.multiple_of_execution_width = p.request.kernel.multipleOfWidth;
        uint32_t facts[3] = {};
        const bool made = nmb_offline_compute_pipeline_facts(&d, facts);
        if (!made)
            why = std::string("no compute prepare: ") + nmb_last_error();
        nmb_offline_function_release(function);
        nmb_offline_library_release(library);
        entry = raw::encodeFactsEntry(p.key, made ? facts : nullptr, why);
        return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
    }
}

}  // namespace switch_compiler
