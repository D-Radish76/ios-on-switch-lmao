// SPDX-License-Identifier: MIT
// The blit encoder's copies as recorded: Metal's rules and plain Metal's answers (RawMetalBlitRules.h) decide each one,
// which the copy engine then moves (RawMetalEncodeBlit.mm). Fills and mipmaps: RawMetalBlitFill.mm.
#import "RawMetalInternal.h"
#import "RawMetalResolveAsk.h"
#import "RawMetalBlitImages.h"
#import "RawMetalBlitRules.h"
#include "raw_blit_planes.h"

namespace {
// How a texture-to-texture copy of neither side held through planes moves its texels (OpCopyTexture's a): its texels, a
// stencil view's stencil into another's (raw::kAspectStencil), or a stencil view's stencil out of or into a texture of
// one-byte texels
uint32_t textureCopyForm(RMTexture *a, RMTexture *b)
{
    const bool fromStencil = a->_descriptor.pixelFormat == MTLPixelFormatX32_Stencil8,
               toStencil = b->_descriptor.pixelFormat == MTLPixelFormatX32_Stencil8;
    if (fromStencil && toStencil)
        return raw::kAspectStencil;
    return fromStencil ? kCopyStencilOut : toStencil ? kCopyStencilIn : raw::kAspectWhole;
}

// A copy one of whose sides is held through planes, as an M4 Max moves it with validation off (raw_blit_planes.h):
// plane 0's bytes counted in the destination's texel (a plain texture's, or plane 0's unit), and between two such
// textures plane 1 too, a byte a texel at each side's plane-0 rows (kCopyPlanes). Each side's region is inside its texture
// (copySideInvalid). Null: the form; else why Metal's validation stops the copy, which past it lands the bytes of a
// texture of another texel size than plane 0's unit transposed into the planes, or reads zeros past the source
// surface's last page
const char *planesCopyForm(RMTexture *a, MTLOrigin so, MTLSize size, RMTexture *b, MTLOrigin dorg, uint32_t &form)
{
    const raw::Texture &s = a->_raw, &d = b->_raw;
    const bool fromPlanes = raw::heldInPlanes(s), intoPlanes = raw::heldInPlanes(d);
    const raw::Texture &plain = fromPlanes ? d : s;
    if (!(fromPlanes && intoPlanes) && (plain.samples > 1 || plain.zeta || plain.blockW > 1 || plain.blockH > 1))
        return "between a texture held through planes and a multisampled, depth or block-compressed one";
    const uint32_t unit = intoPlanes ? raw::planeUnit(d) : d.bpp;
    if (!fromPlanes && s.bpp != unit)
        return "into a texture held through planes from one of another texel size than its plane 0's";
    raw::Texture from, to;
    if (fromPlanes && (!raw::planeBytes(s, unit, from) ||
                       !raw::planeRegionReadable(from, so.x, so.y, size.width, size.height)))
        return "of a region that reads past the end of its surface's last page";
    if (intoPlanes &&
        (!raw::planeBytes(d, unit, to) || dorg.x + size.width > to.width || dorg.y + size.height > to.height))
        return "of a region past a plane's rows";
    form = fromPlanes && intoPlanes ? kCopyPlanes : kCopyPlaneBytes;
    return nullptr;
}

// Whether a copy between texture `d` and buffer `s` is the raw path's (`aspect` the aspect it moves, `o` and `size`
// placed in the level where it writes the texture); false: the copy was skipped or its command buffer failed
bool bufferCopy(__unsafe_unretained RMCommandBuffer *cb, const char *call, id<MTLBuffer> s, id<MTLTexture> d,
                NSUInteger slice, NSUInteger level, MTLOrigin &o, MTLSize &size, NSUInteger offset, NSUInteger row,
                NSUInteger image, MTLBlitOption options, bool intoTexture, int &aspect)
{
    RMBuffer *a = [(id)s isKindOfClass:[RMBuffer class]] ? (RMBuffer *)s : nil;
    aspect = raw::kAspectWhole;
    if (!a) {
        rmCallInvalid(cb, PredCallBufForeign, offset, 0,
                      s ? "a blit of a buffer not made by this device" : "a blit of a nil buffer");
        return false;
    }
    if (!d) {
        rmCallInvalid(cb, PredTexForeign, 0, 0, "a blit of a nil texture");
        return false;
    }
    RMTexture *t = rawTexture(d);
    if (!t) {
        notRawTexture(cb, d);
        return false;
    }
    raw::Texture plane;
    if (t->_raw.converted() && !raw::planeBytes(t->_raw, raw::planeUnit(t->_raw), plane)) {
        failNoImage(cb, call);
        return false;
    }
    if ((aspect = blitAspect(t, options)) == kAspectNoMemory) {
        failNoStencilTemp(cb, call);
        return false;
    }
    if (intoTexture) {
        roundToBlocks(call, t, o);
        cutToLevel(call, t, level, o, size);
    }
    if (const char *invalid =
            bufferCopyInvalid(t, slice, level, o, size, offset, row, image, a->_raw.length, aspect, intoTexture)) {
        invalidBlit(cb, call, invalid);
        return false;
    }
    if (!copySideOk(t, slice, level, o, size)) {
        failNoImage(cb, call);
        return false;
    }
    return true;
}
}  // namespace

@implementation RMBlitEncoder

- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    constexpr const char *call = "copyFromTexture:toTexture:";
    if (!s || !d)
        return rmCallInvalid(_cb, PredTexForeign, 0, 0, "a blit of a nil texture");
    RMTexture *a = rawTexture(s), *b = rawTexture(d);
    uint32_t form = raw::kAspectWhole;
    if (!a || !b)
        return notRawTexture(_cb, a ? d : s);
    flatSlice(call, a, ss);
    if (raw::heldInPlanes(a->_raw) || raw::heldInPlanes(b->_raw)) {
        if (const char *invalid = copySideInvalid(a, ss, sl, so, size) ?: copySideInvalid(b, ds, dl, dorg, size)
                                                                     ?: planesCopyForm(a, so, size, b, dorg, form))
            return invalidBlit(_cb, call, invalid);
        if ((!raw::heldInPlanes(a->_raw) && !copySideOk(a, ss, sl, so, size)) ||
            (!raw::heldInPlanes(b->_raw) && !copySideOk(b, ds, dl, dorg, size)))
            return failNoImage(_cb, call);
    } else {
        cutToLevel(call, a, sl, so, size);
        if (const char *invalid = textureCopyInvalid(a, ss, sl, so, size, b, ds, dl, dorg))
            return invalidBlit(_cb, call, invalid);
        form = textureCopyForm(a, b);
        if ((form == kCopyStencilOut || form == kCopyStencilIn) &&
            blitAspect(form == kCopyStencilOut ? a : b, MTLBlitOptionNone) == kAspectNoMemory)
            return failNoStencilTemp(_cb, call);
        if (!copySideOk(a, ss, sl, so, size) || !copySideOk(b, ds, dl, dorg, size))
            return failNoImage(_cb, call);
    }
    [_cb retain:s];
    [_cb retain:d];
    noteReadAsData(s);
    if ([(id)d isKindOfClass:[RMTexture class]])
        declareGpuWrites((RMTexture *)d);
    Cmd m = cmd(OpCopyTexture);
    m.a = form;
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[11] = {ss,
                            sl,
                            so.x,
                            so.y,
                            so.z,
                            size.width,
                            size.height,
                            size.depth,
                            ds,
                            dl,
                            uint64_t(dorg.x) | uint64_t(dorg.y) << 32};
    _cb->_extra.append(e, e + 11);
    _cb->_extra.push_back(dorg.z);
    [_cb record:m];
}

- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
             sliceCount:(NSUInteger)slices
             levelCount:(NSUInteger)levels
{
    for (NSUInteger k = 0; k < slices; k++)
        for (NSUInteger l = 0; l < levels; l++)
            [self copyFromTexture:s
                      sourceSlice:ss + k
                      sourceLevel:sl + l
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:levelSize(s, sl + l)
                        toTexture:d
                 destinationSlice:ds + k
                 destinationLevel:dl + l
                destinationOrigin:MTLOriginMake(0, 0, 0)];
}

// Metal's whole-texture copy (MTLBlitCommandEncoder.h): every level of a size both have, from the larger's level of the
// smaller's size, and as many slices as both have
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d
{
    NSUInteger sl = 0, dl = 0;
    if (!sameSizeLevels(s, d, sl, dl))
        return rmCallInvalid(_cb, PredCallBlit, 0, 0,
                             "copyFromTexture:toTexture: between textures neither of which has a level of the other's "
                             "first level's size");
    [self copyFromTexture:s
              sourceSlice:0
              sourceLevel:sl
                toTexture:d
         destinationSlice:0
         destinationLevel:dl
               sliceCount:std::min(sliceCount(s), sliceCount(d))
               levelCount:std::min(s.mipmapLevelCount - sl, d.mipmapLevelCount - dl)];
}

- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)value
{
    RMBuffer *t = (RMBuffer *)b;
    if (![(id)t isKindOfClass:[RMBuffer class]])
        return rmCallInvalid(_cb, PredCallBufForeign, NSMaxRange(r), 0,
                             b ? "a fill of a buffer not made by this device" : "a fill of a nil buffer");
    if (NSMaxRange(r) > t->_raw.length) {
        skip(_cb, PredCallBufCopyRange, NSMaxRange(r), t->_raw.length, "a fill past its buffer, clamped to it");
        r.length = NSUInteger(raw::clampedCopy(t->_raw.length, r.location, t->_raw.length, r.location, r.length));
        if (!r.length)
            return;
    }
    [_cb retain:b];
    Cmd m = cmd(OpFillBuffer);
    m.obj = (__bridge void *)b;
    m.a = value;
    m.c = r.location;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(r.length);
    [_cb record:m];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
          sourceOffset:(NSUInteger)so
              toBuffer:(id<MTLBuffer>)d
     destinationOffset:(NSUInteger)dof
                  size:(NSUInteger)n
{
    RMBuffer *a = (RMBuffer *)s, *b = (RMBuffer *)d;
    if (![(id)a isKindOfClass:[RMBuffer class]] || ![(id)b isKindOfClass:[RMBuffer class]])
        return rmCallInvalid(_cb, PredCallBufForeign, so + n, dof + n,
                             s && d ? "a copy of a buffer not made by this device" : "a copy of a nil buffer");
    if (so + n > a->_raw.length || dof + n > b->_raw.length) {
        skip(_cb, PredCallBufCopyRange, so + n, dof + n, "a copy past its buffers, clamped to both");
        n = NSUInteger(raw::clampedCopy(a->_raw.length, so, b->_raw.length, dof, n));
        if (!n)
            return;
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = 0;
    m.c = so;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(dof);
    _cb->_extra.push_back(n);
    [_cb record:m];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    [self copyFromBuffer:s
               sourceOffset:so
          sourceBytesPerRow:row
        sourceBytesPerImage:image
                 sourceSize:size
                  toTexture:d
           destinationSlice:ds
           destinationLevel:dl
          destinationOrigin:dorg
                    options:MTLBlitOptionNone];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
                options:(MTLBlitOption)options
{
    int aspect;
    if (!bufferCopy(_cb, "copyFromBuffer:toTexture:", s, d, ds, dl, dorg, size, so, row, image, options, true, aspect))
        return;
    [_cb retain:s];
    [_cb retain:d];
    if ([(id)d isKindOfClass:[RMTexture class]])
        declareGpuWrites((RMTexture *)d);
    Cmd m = cmd(OpCopyBufferToTexture);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = uint32_t(aspect);
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        so, row, image, size.width, size.height, size.depth, ds, dl, uint64_t(dorg.x) | uint64_t(dorg.y) << 32, dorg.z};
    _cb->_extra.append(e, e + 10);
    [_cb record:m];
}

- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
{
    [self copyFromTexture:s
                     sourceSlice:ss
                     sourceLevel:sl
                    sourceOrigin:so
                      sourceSize:size
                        toBuffer:d
               destinationOffset:dof
          destinationBytesPerRow:row
        destinationBytesPerImage:image
                         options:MTLBlitOptionNone];
}

- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
                     options:(MTLBlitOption)options
{
    int aspect;
    if (!bufferCopy(_cb, "copyFromTexture:toBuffer:", d, s, ss, sl, so, size, dof, row, image, options, false, aspect))
        return;
    [_cb retain:s];
    [_cb retain:d];
    noteReadAsData(s);
    Cmd m = cmd(OpCopyTextureToBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = uint32_t(aspect);
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        ss, sl, uint64_t(so.x) | uint64_t(so.y) << 32, so.z, size.width, size.height, size.depth, dof, row, image};
    _cb->_extra.append(e, e + 10);
    [_cb record:m];
}

- (void)synchronizeResource:(id<MTLResource>)r
{
    (void)r;  // shared memory: nothing to make visible
}

- (void)synchronizeTexture:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l
{
    (void)t;
    (void)s;
    (void)l;
}

- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t
{
    (void)t;  // pitch-linear memory either way
}

- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t
{
    (void)t;
}

@end
