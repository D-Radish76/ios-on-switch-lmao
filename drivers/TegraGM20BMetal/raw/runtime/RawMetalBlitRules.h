// SPDX-License-Identifier: MIT
// A blit's sides as Metal takes them (RawMetalBlitRules.mm): the rules MTLBlitCommandEncoder.h states, and plain Metal's
// answers past them that m2v measured (copy_plan.rs, blit_plan.rs), for the blit encoder's copies, fills and mipmaps
#pragma once
#import "RawMetalInternal.h"

// blitAspect's answer where a Depth32Float_Stencil8 texture has no memory for its stencil temp
constexpr int kAspectNoMemory = -1;

// A texture-to-texture copy's form (OpCopyTexture's a) past raw::kAspectWhole and raw::kAspectStencil (a stencil view into
// another): a stencil view's stencil into a texture of one-byte texels, the other way, both planes of two textures held
// through planes, and plane 0's bytes of a side held through planes (raw_blit_planes.h planeBytes)
constexpr uint32_t kCopyStencilOut = 3, kCopyStencilIn = 4, kCopyPlanes = 5, kCopyPlaneBytes = 6;

MTLSize levelSize(id<MTLTexture> t, NSUInteger level);
NSUInteger sliceCount(id<MTLTexture> t);
// A copy's texel origin and size as `t`'s elements (raw::toElements); false where the origin is not a whole block's
bool inElements(RMTexture *t, MTLOrigin &o, MTLSize &size);
// In elements (inElements)
bool copySideOk(RMTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size);
// A buffer side's planes: `image` bytes apart, each on the rows' terms (bufferRows), the last inside the buffer
bool bufferPlanes(uint64_t offset, uint64_t row, uint64_t image, MTLSize size, uint32_t bpp, uint64_t length,
                  bool intoTexture);
// The aspect (raw::kAspect*) a copy between `t` and a buffer moves under `options`, as plain Metal moves it
int blitAspect(RMTexture *t, MTLBlitOption options);
// The bytes of each texel a copy between `t` and a buffer moves in the buffer
uint32_t blitUnit(RMTexture *t, int aspect);

// The texture of this device the raw path holds, else nil
RMTexture *rawTexture(id<MTLTexture> t);
// A blit of a texture another device made: Metal's validation error, the command buffer fails (rmCallInvalid)
void notRawTexture(RMCommandBuffer *cb, id<MTLTexture> t);

// A blit Metal's validation stops, recorded as nothing, one line a process (raw:blit.invalid)
void skipInvalidBlit(const char *call, const char *why);
// A colour fill of a texture held through planes: undefined on Apple's GPU (an M4 Max writes plane 0's region with a value
// that ignores the colour and follows the process's earlier fills), recorded as nothing, one line a process
// (raw:blit.undefined)
void skipUndefinedFill(const char *call);
// A blit Metal's rules take whose side the raw path holds no image of (the facade's own fault, never a client's): its
// command buffer fails MTLCommandBufferErrorInternal, one line (raw:blit.image)
void failNoImage(__unsafe_unretained RMCommandBuffer *cb, const char *call);
// No memory for a Depth32Float_Stencil8 texture's stencil temp: MTLCommandBufferErrorOutOfMemory, one line
// (raw:blit.stencil_memory)
void failNoStencilTemp(__unsafe_unretained RMCommandBuffer *cb, const char *call);
// Why one side of a copy breaks Metal's rules, null where it keeps them
const char *copySideInvalid(RMTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size);
// Why a texture-to-texture copy breaks Metal's rules: another sample count, formats of other copy units, a side outside
// its texture
const char *textureCopyInvalid(RMTexture *a, NSUInteger ss, NSUInteger sl, MTLOrigin so, MTLSize size, RMTexture *b,
                               NSUInteger ds, NSUInteger dl, MTLOrigin dorg);
// Why a copy between texture `t` and a buffer of `length` bytes breaks Metal's rules
const char *bufferCopyInvalid(RMTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size,
                              uint64_t offset, uint64_t row, uint64_t image, uint64_t length, int aspect,
                              bool intoTexture);
// The levels a whole-texture copy starts at: the smaller's first, and the larger's of its size. False: neither has a level
// of the other's size.
bool sameSizeLevels(id<MTLTexture> s, id<MTLTexture> d, NSUInteger &sl, NSUInteger &dl);
