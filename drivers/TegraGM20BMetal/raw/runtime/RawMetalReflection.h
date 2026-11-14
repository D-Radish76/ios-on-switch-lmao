// SPDX-License-Identifier: MIT
// A state's reflection without m2v: the blob (raw_reflection_blob.h) the pack holds of it (PackReflections), else the one
// the system shader compiler makes (RequestKind::Reflection), read into objects that answer as Metal's MTLArgument,
// MTLBinding, MTLStructType, MTLArrayType, MTLPointerType and MTLTextureReferenceType do. Asked only with the
// ArgumentInfo or BufferTypeInfo creation option, as Metal (and m2v) answer nil without either.
#pragma once
#import <Metal/Metal.h>
#include <cstddef>
#include <cstdint>

@class RMPipeline;
@class RMComputeState;

// The blob of `stages` stages as Metal's reflection: a compute state's (one stage) or a render state's (five); nil where
// it is not one whole
id rmReflectionOf(const uint8_t *bytes, size_t length, bool compute);

// `p`'s reflection under creation options `o`, of its descriptor `d`; nil where none was asked or none could be made (an
// error line names why)
MTLAutoreleasedRenderPipelineReflection rmRenderReflection(RMPipeline *p, MTLRenderPipelineDescriptor *d,
                                                           MTLPipelineOption o);
// `s`'s reflection under `o` into `*reflection` (nil where none was asked, or for a state no request can name: linked
// functions, a stage input, another device's function, as m2v answered); false with Metal's error where the pack and the
// compiler give none, which fails the creation as m2v's compute prepare did
bool rmComputeReflection(RMComputeState *s, MTLPipelineOption o, MTLAutoreleasedComputePipelineReflection *reflection,
                         NSError **error);
