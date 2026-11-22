// SPDX-License-Identifier: MIT
// The Metal side of decoded formats (raw_decoded.h): their uploads, recorded and encoded as decode passes, and their
// readbacks, which move nothing.
#pragma once
#import "RawMetalInternal.h"
#include "raw_decoded.h"

// The format `t` decodes its uploads by, null for a texture of any other format
const raw::DecodedFormat *rmDecoded(RMTexture *t);
// A copyFromBuffer:toTexture: into decoded `t` as Metal's rules take it, `size` cut to its level (whose last blocks
// hold its edge texels); false: refused with one line, nothing recorded
bool rmDecodedCopyRecorded(RMCommandBuffer *cb, const char *call, RMBuffer *b, RMTexture *t, NSUInteger slice,
                           NSUInteger level, MTLOrigin o, MTLSize &size, NSUInteger offset, NSUInteger row,
                           NSUInteger image);
// Its decode passes: plane by plane of a 3D level, `image` bytes apart, into the stored image of `t`'s levels owner
bool rmEncodeDecodedCopy(raw::CommandBuffer *rc, RMBuffer *b, RMTexture *t, uint64_t offset, uint64_t row,
                         uint64_t image, const uint32_t size[3], uint32_t slice, uint32_t level,
                         const uint32_t origin[3], const char **why);
// replaceRegion of decoded `t`: the caller's blocks once into a buffer of the device's, decoded by a blit of an
// internal command buffer committed after them. False: `t` is of another format
bool rmDecodedReplace(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, const void *bytes, NSUInteger row,
                      NSUInteger image);
// A readback (getBytes, a blit into a buffer) of decoded `t`: nothing moved, one line. False: `t` is of another format
bool rmDecodedReadback(RMTexture *t, const char *call);
