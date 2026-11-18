// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"

// raw_texture_shape.h storedSamples
using RMStoredSamples = raw::shapes::StoredSamples;

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d);

// The pass's own `count` sample positions (getSamplePositions:nil count:0) for a pass of `samples` samples
// (raw_sample_positions.h); false: ones the raw path keeps Metal's refusal of
bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, raw::SamplePositions &out);
