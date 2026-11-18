// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <cstdint>

// ---- refusal diagnostics (r10-rawsweep): which predicate a command buffer hit, with its values ----
// Every refusal site names its predicate. A command buffer keeps the first one it hit, with its values. The first hit of each
// predicate in the process logs one line with its values (and, for a resource refused when made, the resource's details).
// Hot path: a relaxed load; strings only on a predicate's first hit.
enum RMPred : uint8_t {
    PredNone = 0,
    PredTexForeign,  // a texture this device did not make
    // passes
    PredPassAttFormat,  // a color attachment not kept or not renderable: a = format, b = attachment index | kind << 8
    PredPassAttLevel,   // slice, depth plane or resolve, or a level of a texture with no chain
    PredPassDepthStencil,  // a = depth format, b = stencil format
    // states and calls
    PredStateForeign,
    PredStateNotPackable,  // a state whose functions are not this device's (neither in the pack nor made by the runtime)
    PredStateNotInPack,
    PredCallVisibility,
    PredCallBufForeign,      // a = stage << 8 | index (0xffff: an index buffer)
    PredCallSamplerForeign,  // a sampler the raw path does not express (its reason logged when it was made)
    PredCallBufCopyRange,
    PredCompute,           // a kernel's buffer, texture or sampler not made by this device
    PredComputeKernel,     // no pack kernel at the dispatch's shape, memory, textures and samplers
    PredComputeImage,      // a texture sampled through an image of its own: a = slot
    PredComputePredicate,  // predication raw cannot run (a misused record, a word outside a raw buffer): the command buffer fails; a = kind or offset
    PredEvent,
    // found while encoding (the raw encode ran up to here and was thrown away)
    PredEncPlanarClamp,
    PredEncPlanarRT,
    PredEncPassRender,
    PredEncKernel,
    PredEncIndexedRange,  // a = offset, b = index size
    PredEncColorFormats,  // a = attachment, b = pass format, c in the line: state format
    PredEncNoTic,
    PredEncDepthStencil,  // a = the state's depth format, b = the pass's | a state that tests without the attachment << 32
    PredEncSamples,       // a = the state's samples, b = the pass's | the state's shading passes << 32
    PredEncNoChannel,     // the kernel refused the channel or its context (logged with its answer)
    PredEncGeometry,      // a = primitive, b = count | instances << 32; no native raster-order plan
    PredEncStorageView,  // a = attachment, b = format | width << 32: no storage view and no barrier form to take
    PredPassRotated,     // a = raw::RotatedRefusal, b = the rotation (a pass's, or the state's at encode)
    PredPassPositions,   // sample positions Metal takes and draws nothing defined for: a = their count, b = the samples
    PredCount
};

static_assert(PredCount <= 64, "one mask word");
static constexpr const char *kPredNames[] = {"-",
                                             "tex.foreign",
                                             "pass.fmt",
                                             "pass.level",
                                             "pass.ds",
                                             "state.foreign",
                                             "state.notpack",
                                             "state.pack",
                                             "call.vis",
                                             "call.buf",
                                             "call.sampler",
                                             "call.bufcopy",
                                             "compute",
                                             "compute.nokernel",
                                             "compute.image",
                                             "compute.predicate",
                                             "event",
                                             "enc.planarclamp",
                                             "enc.planarrt",
                                             "enc.pass",
                                             "enc.kernel",
                                             "enc.idxrange",
                                             "enc.colorfmt",
                                             "enc.notic",
                                             "enc.ds",
                                             "enc.samples",
                                             "enc.nochannel",
                                             "enc.geometry",
                                             "enc.storage",
                                             "pass.rotated",
                                             "pass.positions"};
static_assert(sizeof kPredNames / sizeof *kPredNames == PredCount, "a name for each predicate");

// The predicates whose command buffer fails, its work run nowhere, with MTLCommandBufferErrorDomain's code (0: not one):
// the kernel refusing a submission or the channel (raw/plan/predicates.tsv cb-error). The client's errors (call.bufcopy,
// enc.idxrange, enc.colorfmt, enc.samples, pass.positions) complete as Apple's driver completes them
// (raw_client_errors.h), each logged once.
constexpr NSInteger predFailure(RMPred p)
{
    switch (p) {
    case PredEncKernel:
    case PredEncNoChannel:
        return MTLCommandBufferErrorInternal;
    default:
        return 0;
    }
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
extern std::atomic<uint64_t> gPredFailed[PredCount];  // command buffers each predicate failed (predFailure)
#endif
extern std::atomic<uint64_t> gPredLogged;

static inline bool predLogged(RMPred p)
{
    return gPredLogged.load(std::memory_order_relaxed) >> p & 1;
}

// The predicate's first hit in the process: one line with its values (`detail`: the resource's, when it was made)

void logPred(RMPred p, uint64_t a, uint64_t b, NSString *detail);
