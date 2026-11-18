// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"

// raw_texture_shape.h storedSamples
using RMStoredSamples = raw::shapes::StoredSamples;

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d);

// m2v's twin of stored one-sample depth/stencil texture `t`, made at the first replay that needs it: NVK's image, with
// `t`'s texels copied into it and `t` over it from then on (raw::Device::moveOwnedZeta); nil, *why, where it cannot be
id storedZetaTwin(RMTexture *t, id<MTLDevice> m2v, const char **why);

// The pass's own `count` sample positions (getSamplePositions:nil count:0) for a pass of `samples` samples
// (raw_sample_positions.h); false: ones the raw path keeps Metal's refusal of
bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, raw::SamplePositions &out);
