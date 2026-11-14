// SPDX-License-Identifier: MIT
// See compiler_facts.h.
#import "compiler_facts.h"

#import <Foundation/Foundation.h>
#include <vector>
#include "MetallibSlice.h"
#include "TegraGM20BCompilerABI.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/RawPipelineKey.h"
#include "../raw/runtime/raw_compute_facts_entry.h"
#include "../translate/NativeMetalTranslate.h"

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

}  // namespace

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
