// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalObjects.h"
#include "raw_compute_lookup.h"

@interface RMComputeState : RMObject {
@public
    // its stage input's layouts are in its key (raw::stageInText), which no facts or reflection request carries
    bool _stageIn;
    raw::AirHash _air;
    std::string _constants;
    uint32_t _maxThreads, _threadLimit;
    bool _multipleOfWidth;
    raw::ComputeKernelCache<raw::ComputeState> _kernels;
    MTLComputePipelineDescriptor *_direct;
    RMFunction *_function;         // the facade's, which a compile request names
    NSString *_libraryName;        // the library function it was made from, which an archive key names
    raw::PackComputeFacts _facts;  // the pack's answers, or the shader compiler's
    id<MTLComputePipelineState> _compatibility;
    std::once_flag _twinOnce;
    const char *_m2vMissing;  // what of it m2v has no form of (rmM2vMissingKernel), null: nothing
    std::once_flag _writtenOnce;
    uint32_t _texturesWritten;
}
@end

// RawMetalReflection.mm: the texture slots `s`'s kernel may store (a binding the reflection gives write access), read
// once from its reflection, the pack's else the shader compiler's; every slot where neither answers
uint32_t rmComputeTexturesWritten(RMComputeState *s);
