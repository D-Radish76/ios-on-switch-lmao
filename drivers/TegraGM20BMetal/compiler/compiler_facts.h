// SPDX-License-Identifier: MIT
// A compute state's facts (raw_compile_request.h RequestKind::Facts) as the pack generator makes its PackComputeFacts
// (raw/pack/offline_metal.mm kernelFacts): the translator alone over the library's AIR slice, with no compile context,
// so a state's creation in a client that the pack lacks needs neither m2v nor a compile of its own.
#pragma once
#include <cstdint>
#include <string>
#include "compiler_request.h"

namespace switch_compiler {

// `p`'s facts or refusal as the store keeps it (raw_compute_facts_entry.h), its system library read under `systemRoot`;
// TegraGM20BCompilerABI.h's result
int32_t makeFacts(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why);

}  // namespace switch_compiler
