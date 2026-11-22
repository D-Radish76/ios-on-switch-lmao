// SPDX-License-Identifier: MIT
// newTextureViewWithPixelFormat: of a raw texture: the same memory under another format's header, or nil with one line
#import "RawMetalInternal.h"
#import "RawMetalXrPlanes.h"
#include "raw_decoded.h"
#include "raw_ycbcr.h"

int viewHeaderType(MTLTextureType type)
{
    switch (type) {
    case MTLTextureType1D:
        return raw::fx::kTexOneD;
    case MTLTextureType1DArray:
        return raw::fx::kTexOneDArray;
    case MTLTextureType2D:
        return raw::fx::kTexTwoD;
    case MTLTextureType2DArray:
        return raw::fx::kTexTwoDArray;
    case MTLTextureTypeCube:
        return raw::fx::kTexCube;
    case MTLTextureTypeCubeArray:
        return raw::fx::kTexCubeArray;
    case MTLTextureType3D:
        return raw::fx::kTexThreeD;
    case MTLTextureType2DMultisample:
        return raw::fx::kTexTwoD;
    case MTLTextureType2DMultisampleArray:
        return raw::fx::kTexTwoDArray;
    default:
        return -1;
    }
}

// Metal's verdict on viewing a texture of `source` as `view` where the raw path's views do not already stop it (m2v
// view_plan.rs check_format, measured on an M3 Max and an M4): an extended-range format, of one plane or two, views as
// itself and its sRGB twin alone, and no other format views as one
static const char *viewFormatRefusal(uint32_t source, uint32_t view)
{
    if (const char *why = raw::shapes::formatRefusal(view))
        return why;
    const auto extended = [](uint32_t f) { return f >= 550 && f <= 555; };
    if ((extended(source) || extended(view)) && view != source && view != (source ^ 1))
        return "an extended-range format views as itself and its sRGB twin alone";
    // Metal makes these (an M4 Max: ASTC sRGB and linear, LDR and HDR of one footprint), each reading the blocks by
    // another decode, of which the texture keeps no image, nor the blocks to make one
    if (!raw::decodedViewShared(source, view))
        return "a view decoding the texture's blocks another way than it does: the texture holds its own decode alone";
    return nullptr;
}

// A view that was not made: nil with one line (Metal's stop, or no memory or header for it)
static id<MTLTexture> unmadeView(RMTexture *of, RMTexture *v, const char *why)
{
    SWITCH_REFUSE_DETAIL("raw", "tex.view", v->_descriptor.pixelFormat,
                         "no view of format %lu, type %lu, levels %lu, of a %lux%lu texture of format %lu: %s: nil "
                         "(MTLTexture.h newTextureViewWithPixelFormat:, nullable)",
                         (unsigned long)v->_descriptor.pixelFormat, (unsigned long)v->_descriptor.textureType,
                         (unsigned long)v->_descriptor.mipmapLevelCount, (unsigned long)of->_descriptor.width,
                         (unsigned long)of->_descriptor.height, (unsigned long)of->_descriptor.pixelFormat, why);
    return nil;
}

// The raw format a view in `format` of `owner`'s memory takes: the owner's own where the view keeps its descriptor's
// format (a converted texture's image is of another format than its descriptor's); an RGB10A8_2P_XR10 held as BGR10_XR's
// words viewed as its sRGB twin, BGR10_XR_sRGB's (an M4 Max reads them so, alpha 0: m2v-textures4 probes/reads.mm view)
static uint32_t viewRawFormat(RMTexture *owner, MTLPixelFormat format)
{
    if (xrWordsTexture(owner) && raw::twoPlaneXr(uint32_t(format)))
        return xrWordsFormat(uint32_t(format));
    if (const raw::DecodedFormat *f = raw::decodedFormat(uint32_t(format)))
        return f->stored;
    return format == owner->_descriptor.pixelFormat ? owner->_raw.format : uint32_t(format);
}

// A view of words held so reads alpha 0 through its stand-in, as the texture does
static id<MTLTexture> madeView(RMTexture *owner, RMTexture *v)
{
    if (xrWordsTexture(owner) && v->_raw.standIn)
        readAlphaZero(*v->_raw.standIn);
    return (id<MTLTexture>)v;
}

@implementation RMTexture (Views)

// A view: the same memory under another format's header, with the stand-in and pool image its draws sample
// (raw::Device::createView, createPartView), always over the memory's owner (the view's levels owner), so a view of a view
// names its part of that memory
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
{
    const NSUInteger allSlices = raw::textureLayers(uint32_t(_descriptor.textureType),
                                                    uint32_t(std::min<NSUInteger>(_descriptor.arrayLength, 2048)));
    if (type == _descriptor.textureType && !levels.location && levels.length == _descriptor.mipmapLevelCount &&
        !slices.location && slices.length == allSlices)
        return [self newTextureViewWithPixelFormat:format];
    RMTexture *v = [RMTexture new];
    v->_device = _device;
    v->_descriptor = [_descriptor copy];
    v->_descriptor.pixelFormat = format;
    v->_descriptor.textureType = type;
    v->_descriptor.mipmapLevelCount = levels.length;
    const NSUInteger shift = std::min<NSUInteger>(levels.location, 63);
    v->_descriptor.width = std::max<NSUInteger>(_descriptor.width >> shift, 1);
    v->_descriptor.height = std::max<NSUInteger>(_descriptor.height >> shift, 1);
    if (type == MTLTextureType3D)
        v->_descriptor.depth = std::max<NSUInteger>(_descriptor.depth >> shift, 1);
    const bool viewCube = type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray;
    v->_descriptor.arrayLength = std::max<NSUInteger>(viewCube ? slices.length / 6 : slices.length, 1);
    v->_kind = RMTextureView;
    v->_parent = self;
    v->_viewPart = true;
    v->_viewLevels = levels;
    v->_viewSlices = slices;
    v->_levelsOwner = _levelsOwner ?: self;
    v->_levelsBase = _levelsBase + uint32_t(levels.location);
    v->_slicesBase = _slicesBase + uint32_t(slices.location);
    const int header = viewHeaderType(type);
    const bool multisampled = type == MTLTextureType2DMultisample || type == MTLTextureType2DMultisampleArray;
    const bool inParent = levels.location < _descriptor.mipmapLevelCount && levels.length &&
                          levels.length <= _descriptor.mipmapLevelCount - levels.location &&
                          slices.location < allSlices && slices.length && slices.length <= allSlices - slices.location;
    const char *why = viewFormatRefusal(uint32_t(_descriptor.pixelFormat), uint32_t(format));
    if (!why && (!inParent || header < 0 || multisampled != (_raw.samples > 1)))
        why = "levels, slices or a type its texture does not have";
    if (!why && _rawOk)
        v->_rawOk = gRM->raw->createPartView(v->_levelsOwner->_raw, viewRawFormat(v->_levelsOwner, format),
                                             uint32_t(header), v->_levelsBase, uint32_t(levels.length), v->_slicesBase,
                                             uint32_t(slices.length), v->_raw);
    if (v->_rawOk)
        return madeView(v->_levelsOwner, v);
    return unmadeView(self, v,
                      why ?: _raw.xrPlanes ? "a two-plane extended-range texture viewed as another type than its own"
                                           : "another texel size, a part of a pitch or multisampled image, or no "
                                             "memory or header for it");
}

// A swizzled view: the view, its header reading its channels so
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
                                        swizzle:(MTLTextureSwizzleChannels)swizzle
{
    return swizzled([self newTextureViewWithPixelFormat:format textureType:type levels:levels slices:slices], swizzle);
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
    RMTexture *v = [RMTexture new];
    v->_device = _device;
    v->_descriptor = [_descriptor copy];
    v->_descriptor.pixelFormat = format;
    v->_kind = RMTextureView;
    v->_parent = self;
    v->_levelsOwner = _levelsOwner ?: self;
    v->_levelsBase = _levelsBase;
    v->_slicesBase = _slicesBase;
    RMTexture *owner = v->_levelsOwner;
    MTLTextureDescriptor *o = owner->_descriptor;
    const bool whole = !_levelsBase && !_slicesBase && _descriptor.textureType == o.textureType &&
                       _descriptor.mipmapLevelCount == o.mipmapLevelCount && _descriptor.arrayLength == o.arrayLength;
    const char *why = viewFormatRefusal(uint32_t(_descriptor.pixelFormat), uint32_t(format));
    if (!why && _raw.memoryless)
        why = "a memoryless texture takes no view";
    if (!why && _rawOk) {
        // a depth texture's X32_Stencil8 view reads its own header's stencil (createZetaView)
        if (_raw.zeta || whole)
            v->_rawOk = gRM->raw->createView((_raw.zeta ? self : owner)->_raw, viewRawFormat(owner, format), v->_raw);
        else
            v->_rawOk = gRM->raw->createPartView(
                owner->_raw, viewRawFormat(owner, format), uint32_t(viewHeaderType(_descriptor.textureType)),
                _levelsBase, uint32_t(_descriptor.mipmapLevelCount), _slicesBase,
                raw::textureLayers(uint32_t(_descriptor.textureType),
                                   uint32_t(std::min<NSUInteger>(_descriptor.arrayLength, 2048))),
                v->_raw);
    }
    if (v->_rawOk)
        return madeView(owner, v);
    // a converted YCbCr texture as its _sRGB twin: the same planes converted the twin's way into an image of its own (m2v
    // view_plan.rs ycbcr_twins: Metal reads the planes again, its other conversion)
    if (!why && _rawOk && owner->_raw.ycbcr && owner->_surface && raw::isYcbcrFormat(uint32_t(format))) {
        MTLTextureDescriptor *d = [owner->_descriptor copy];
        d.pixelFormat = format;
        RMTexture *converted = (RMTexture *)[(id<MTLDevice>)_device newTextureWithDescriptor:d
                                                                                 iosurface:owner->_surface
                                                                                     plane:owner->_plane];
        if (converted && converted->_rawOk) {
            converted->_parent = self;
            return (id<MTLTexture>)converted;
        }
    }
    return unmadeView(self, v, why ?: "another texel size or format class, or no memory or header for it");
}

@end
