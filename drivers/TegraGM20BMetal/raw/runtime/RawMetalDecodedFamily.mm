// SPDX-License-Identifier: MIT
// A decoded texture that keeps its blocks (raw_decoded.h keepsBlocks) and its views of other forms: one set of blocks,
// each form its own decode of them, as Apple's views alias the blocks (an M4 Max: after an upload into the texture or a
// view, each reads its own decode of the new blocks). Every upload into any of them writes the kept copy and decodes
// into the texture and every live view of another form in the upload's own job; a view's image is made once, when the
// view is, by a blit of the kept blocks into it. Nothing is decoded at a bind or a draw.
// Ownership: the texture holds its kept blocks (an RMBuffer of the device's) and its image; a view of another form
// holds its own image, and its parent, which keeps the texture alive. A command buffer that decodes into a view retains
// it, so its image outlives every job that writes it. The last view going takes its image; the texture going takes the
// blocks and its own image.
#import "RawMetalDecoded.h"
#import "RawMetalBlitImages.h"
#import "RawMetalBlitRules.h"
#include <algorithm>
#include "raw_decode_pass.h"

namespace {

// The texture's extent as its kept blocks are laid out: its width, height, 3D depth and layers
struct Extent {
    uint32_t width, height, depth, layers;
};

Extent extentOf(RMTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    return {uint32_t(d.width), uint32_t(d.height), d.textureType == MTLTextureType3D ? uint32_t(d.depth) : 1u,
            raw::textureLayers(uint32_t(d.textureType), uint32_t(std::min<NSUInteger>(d.arrayLength, 2048)))};
}

raw::KeptLevel keptLevelOf(RMTexture *owner, uint32_t level)
{
    const Extent e = extentOf(owner);
    return raw::keptLevel(*rmDecoded(owner), e.width, e.height, e.depth, e.layers, level);
}

// Whether member `m` of `owner`'s family holds level `level` and layer or plane `z` of the blocks; its own level and
// layer of them where it does
bool holds(RMTexture *m, RMTexture *owner, uint32_t level, uint32_t z, uint32_t &ownLevel, uint32_t &ownLayer)
{
    if (m == owner) {
        ownLevel = level, ownLayer = z;
        return true;
    }
    const bool volume = owner->_raw.type == raw::fx::kTexThreeD;
    const uint32_t layers = raw::textureLayers(uint32_t(m->_descriptor.textureType),
                                               uint32_t(std::min<NSUInteger>(m->_descriptor.arrayLength, 2048)));
    if (level < m->_levelsBase || level - m->_levelsBase >= m->_descriptor.mipmapLevelCount ||
        (!volume && (z < m->_slicesBase || z - m->_slicesBase >= layers)))
        return false;
    ownLevel = level - m->_levelsBase;
    ownLayer = volume ? z : z - m->_slicesBase;
    return true;
}

// The image a member's decode passes draw into: the texture's own, or a view's of its own form
const raw::Texture &imageOf(RMTexture *m)
{
    return m->_ownForm ? m->_raw : (m->_levelsOwner ?: m)->_raw;
}

// `m` held by `cb` until it completes, as the objects its tape names are: an encode decodes into it, so its image must
// outlive the job. The tape's resource plan, which the encode walks, is left as it is.
void holdUntilComplete(RMCommandBuffer *cb, RMTexture *m)
{
    if (cb->_ownerKeys.insert((__bridge void *)m, true).second)
        cb->_owners.push_back(m);
}

}  // namespace

RMTexture *rmFamilyOwner(RMTexture *t)
{
    RMTexture *owner = t ? (t->_levelsOwner ?: t) : nil;
    return owner && owner->_keptBlocks ? owner : nil;
}

bool rmKeepDecodedBlocks(RMTexture *t)
{
    const raw::DecodedFormat *f = rmDecoded(t);
    if (!f || !raw::keepsBlocks(f->format))
        return true;
    const Extent e = extentOf(t);
    const uint64_t bytes =
        raw::keptBytes(*f, e.width, e.height, e.depth, e.layers, uint32_t(t->_descriptor.mipmapLevelCount));
    RMBuffer *b = (RMBuffer *)[(id<MTLDevice>)t->_device newBufferWithLength:NSUInteger(bytes)
                                                                     options:MTLResourceStorageModeShared];
    if (!b)
        return false;
    // a new texture's blocks are zeros, which its readbacks give back and its views decode
    memset(b->_raw.cpu, 0, size_t(bytes));
    t->_keptBlocks = b;
    return true;
}

void rmForgetForm(RMTexture *view)
{
    RMTexture *owner = view->_levelsOwner;
    rmExcludingSubmissions([&] {
        owner->_forms.erase(std::remove(owner->_forms.begin(), owner->_forms.end(), view), owner->_forms.end());
        return true;
    });
}

id<MTLTexture> rmFormView(RMTexture *of, MTLPixelFormat format, MTLTextureType type, NSRange levels, NSRange slices)
{
    RMTexture *owner = of->_levelsOwner ?: of;
    MTLTextureDescriptor *d = [of->_descriptor copy];
    d.pixelFormat = format;
    d.textureType = type;
    d.mipmapLevelCount = levels.length;
    const NSUInteger shift = std::min<NSUInteger>(levels.location, 63);
    d.width = std::max<NSUInteger>(of->_descriptor.width >> shift, 1);
    d.height = std::max<NSUInteger>(of->_descriptor.height >> shift, 1);
    if (type == MTLTextureType3D)
        d.depth = std::max<NSUInteger>(of->_descriptor.depth >> shift, 1);
    const bool cube = type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray;
    d.arrayLength = std::max<NSUInteger>(cube ? slices.length / 6 : slices.length, 1);
    RMTexture *v = [RMTexture new];
    v->_device = of->_device;
    v->_descriptor = d;
    v->_kind = RMTextureView;
    v->_parent = of;
    v->_viewPart = true;
    v->_viewLevels = levels;
    v->_viewSlices = slices;
    v->_levelsOwner = owner;
    v->_levelsBase = of->_levelsBase + uint32_t(levels.location);
    v->_slicesBase = of->_slicesBase + uint32_t(slices.location);
    v->_ownForm = true;
    raw::shapes::TextureShape s = textureShape(d);
    s.ownMemory = true;
    s.storage = raw::shapes::kPrivate;
    // a form the identity makes no texture of (ASTC HDR on 21H461) takes no view either
    if (const char *why = raw::shapes::identityRefusal(s, raw::answers::kThisKind)) {
        SWITCH_REFUSE_DETAIL("raw", "tex.decoded_form", uintptr_t(why),
                             "no view of format %lu of a format %lu texture: nil, as the identity's Metal makes none "
                             "(%s; MTLTexture.h newTextureViewWithPixelFormat:, nullable)",
                             (unsigned long)format, (unsigned long)owner->_descriptor.pixelFormat, why);
        return nil;
    }
    const raw::shapes::Route route = raw::shapes::route(s);
    v->_rawOk = route != raw::shapes::Route::None && raw::shapes::create(*gRM->raw, s, route, true, 0, v->_raw);
    if (!v->_rawOk) {
        SWITCH_REFUSE_DETAIL("raw", "tex.decoded_form", uint32_t(format),
                             "no view of format %lu of a %lux%lu format %lu texture: no memory or header for its own "
                             "decode of the blocks: nil (MTLTexture.h newTextureViewWithPixelFormat:, nullable)",
                             (unsigned long)format, (unsigned long)owner->_descriptor.width,
                             (unsigned long)owner->_descriptor.height, (unsigned long)owner->_descriptor.pixelFormat);
        return nil;
    }
    rmExcludingSubmissions([&] {
        owner->_forms.push_back(v);
        return true;
    });
    // its texels: the kept blocks blitted into it a level and a layer at a time, each decoded into it alone
    id<MTLCommandBuffer> cb = [rmDecodeQueue((id<MTLDevice>)of->_device) commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    const bool volume = type == MTLTextureType3D;
    const NSUInteger layers = volume ? 1 : raw::textureLayers(uint32_t(type), uint32_t(d.arrayLength));
    for (NSUInteger l = 0; l < levels.length; l++) {
        const raw::KeptLevel k = keptLevelOf(owner, v->_levelsBase + uint32_t(l));
        const MTLSize size = levelSize((id<MTLTexture>)v, l);
        for (NSUInteger z = 0; z < layers; z++)
            [blit copyFromBuffer:(id<MTLBuffer>)owner->_keptBlocks
                       sourceOffset:NSUInteger(k.offset + (volume ? 0 : (v->_slicesBase + z) * k.planeBytes))
                  sourceBytesPerRow:k.rowBytes
                sourceBytesPerImage:NSUInteger(k.planeBytes)
                         sourceSize:size
                          toTexture:(id<MTLTexture>)v
                   destinationSlice:z
                   destinationLevel:l
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [blit endEncoding];
    [cb commit];
    return (id<MTLTexture>)v;
}

bool rmEncodeFamilyUpload(raw::CommandBuffer *rc, RMCommandBuffer *cb, raw::BlitEncoder *&be, RMBuffer *source,
                          uint64_t offset, uint64_t row, uint64_t image, RMTexture *t, const uint32_t size[3],
                          uint32_t slice, uint32_t level, const uint32_t origin[3], bool fill, const char **why)
{
    RMTexture *owner = rmFamilyOwner(t);
    const raw::DecodedFormat &f = *rmDecoded(owner);
    const bool volume = owner->_raw.type == raw::fx::kTexThreeD;
    const uint32_t ownerLevel = t->_levelsBase + level, planes = volume ? std::max(size[2], 1u) : 1u;
    const raw::KeptLevel k = keptLevelOf(owner, ownerLevel);
    const uint32_t bx = origin[0] / f.blockW, by = origin[1] / f.blockH;
    const uint32_t across = raw::decodedBlocks(size[0], f.blockW), down = raw::decodedBlocks(size[1], f.blockH);
    raw::Buffer &kept = owner->_keptBlocks->_raw;
    if (source != owner->_keptBlocks) {
        useBuffer(rc, owner->_keptBlocks, true);
        for (uint32_t p = 0; p < planes; p++) {
            const uint32_t z = volume ? origin[2] + p : t->_slicesBase + slice;
            for (uint32_t r = 0; r < down; r++) {
                const uint64_t from = offset + p * image + r * row, to = k.offset + z * k.planeBytes +
                                                                         uint64_t(by + r) * k.rowBytes +
                                                                         uint64_t(bx) * f.blockBytes;
                if (!source->_raw.wc)
                    rc->clean(source->_raw.cpu + from, uint64_t(across) * f.blockBytes);
                if (!kept.wc)
                    rc->clean(kept.cpu + to, uint64_t(across) * f.blockBytes, true);
                be->copyBytes(source->_raw.va + from, kept.va + to, uint64_t(across) * f.blockBytes);
            }
        }
    }
    std::vector<RMTexture *> members;
    if (fill)
        members.push_back(t);
    else {
        members.push_back(owner);
        members.insert(members.end(), owner->_forms.begin(), owner->_forms.end());
    }
    be->end();
    bool ok = true;
    for (RMTexture *m : members) {
        const raw::DecodedFormat *decoded = rmDecoded(m);
        for (uint32_t p = 0; p < planes && ok && decoded; p++) {
            uint32_t ownLevel, ownLayer;
            if (!holds(m, owner, ownerLevel, volume ? origin[2] + p : t->_slicesBase + slice, ownLevel, ownLayer))
                continue;
            holdUntilComplete(cb, m);
            useTexture(rc, m, true);
            const raw::DecodeSource s{&source->_raw, offset + p * image, uint32_t(row)};
            ok = raw::decodePass(*gRM->raw, *rc, imageOf(m), *decoded, ownLevel, ownLayer, origin[0], origin[1],
                                 size[0], size[1], s, why);
        }
    }
    be = rc->blit();
    // a view of a form GM20B decodes holds the blocks themselves, which the copy engine moves in
    for (RMTexture *m : members) {
        if (!ok || rmDecoded(m))
            continue;
        for (uint32_t p = 0; p < planes; p++) {
            uint32_t ownLevel, ownLayer;
            if (!holds(m, owner, ownerLevel, volume ? origin[2] + p : t->_slicesBase + slice, ownLevel, ownLayer))
                continue;
            holdUntilComplete(cb, m);
            useTexture(rc, m, true);
            raw::Texture into;
            rmBufferCopyImage(m, ownLevel, into, true);
            rmCopyPlanes(be, rc, source->_raw, offset + p * image, row, image, into, origin[0], origin[1], ownLayer,
                         size[0], size[1], 1, true, uint8_t(raw::kAspectWhole));
        }
    }
    return ok;
}

bool rmEncodeFamilyReadback(raw::CommandBuffer *rc, raw::BlitEncoder *be, RMTexture *t, RMBuffer *into, uint64_t offset,
                            uint64_t row, uint64_t image, const uint32_t size[3], uint32_t slice, uint32_t level,
                            const uint32_t origin[3])
{
    RMTexture *owner = rmFamilyOwner(t);
    const raw::DecodedFormat &f = *rmDecoded(owner);
    const bool volume = owner->_raw.type == raw::fx::kTexThreeD;
    const raw::KeptLevel k = keptLevelOf(owner, t->_levelsBase + level);
    const uint32_t bx = origin[0] / f.blockW, by = origin[1] / f.blockH;
    const uint32_t across = raw::decodedBlocks(size[0], f.blockW), down = raw::decodedBlocks(size[1], f.blockH);
    const raw::Buffer &kept = owner->_keptBlocks->_raw;
    useBuffer(rc, owner->_keptBlocks, false);
    useBuffer(rc, into, true);
    for (uint32_t p = 0; p < (volume ? std::max(size[2], 1u) : 1u); p++) {
        const uint32_t z = volume ? origin[2] + p : t->_slicesBase + slice;
        for (uint32_t r = 0; r < down; r++) {
            const uint64_t from = k.offset + z * k.planeBytes + uint64_t(by + r) * k.rowBytes +
                                  uint64_t(bx) * f.blockBytes,
                           to = offset + p * image + r * row;
            if (!into->_raw.wc)
                rc->clean(into->_raw.cpu + to, uint64_t(across) * f.blockBytes, true);
            be->copyBytes(kept.va + from, into->_raw.va + to, uint64_t(across) * f.blockBytes);
        }
    }
    return true;
}

bool rmEncodeFamilyTextureCopy(raw::CommandBuffer *rc, RMCommandBuffer *cb, raw::BlitEncoder *&be, RMTexture *s,
                               uint32_t sourceSlice, uint32_t sourceLevel, const uint32_t from[3], RMTexture *t,
                               uint32_t slice, uint32_t level, const uint32_t origin[3], const uint32_t size[3],
                               const char **why)
{
    RMTexture *owner = rmFamilyOwner(t), *sourceOwner = rmFamilyOwner(s);
    const raw::DecodedFormat &f = *rmDecoded(owner);
    const bool volume = owner->_raw.type == raw::fx::kTexThreeD;
    const raw::KeptLevel k = keptLevelOf(owner, t->_levelsBase + level);
    const uint32_t across = raw::decodedBlocks(size[0], f.blockW), down = raw::decodedBlocks(size[1], f.blockH);
    const uint32_t planes = volume ? std::max(size[2], 1u) : 1u;
    // the blocks into the region of the destination's kept copy: from the source's kept copy, or from the image of a
    // texture GM20B decodes, which is its blocks
    const uint64_t at = k.offset + (volume ? origin[2] : t->_slicesBase + slice) * k.planeBytes +
                        uint64_t(origin[1] / f.blockH) * k.rowBytes + uint64_t(origin[0] / f.blockW) * f.blockBytes;
    useBuffer(rc, owner->_keptBlocks, true);
    if (sourceOwner) {
        useBuffer(rc, sourceOwner->_keptBlocks, false);
        const raw::KeptLevel sk = keptLevelOf(sourceOwner, s->_levelsBase + sourceLevel);
        const bool sourceVolume = sourceOwner->_raw.type == raw::fx::kTexThreeD;
        for (uint32_t p = 0; p < planes; p++)
            for (uint32_t r = 0; r < down; r++) {
                const uint64_t a =
                    sk.offset + (sourceVolume ? from[2] + p : s->_slicesBase + sourceSlice) * sk.planeBytes +
                    uint64_t(from[1] / f.blockH + r) * sk.rowBytes + uint64_t(from[0] / f.blockW) * f.blockBytes;
                be->copyBytes(sourceOwner->_keptBlocks->_raw.va + a,
                              owner->_keptBlocks->_raw.va + at + p * k.planeBytes + r * k.rowBytes,
                              uint64_t(across) * f.blockBytes);
            }
    } else if (s->_rawOk && s->_raw.blockW == f.blockW && s->_raw.blockH == f.blockH) {
        useTexture(rc, s, false);
        raw::Texture image;
        rmBufferCopyImage(s, sourceLevel, image, false);
        rmCopyPlanes(be, rc, owner->_keptBlocks->_raw, at, k.rowBytes, k.planeBytes, image, from[0], from[1],
                     volume ? from[2] : sourceSlice, size[0], size[1], planes, false, uint8_t(raw::kAspectWhole));
    } else {
        *why = "a copy into a texture that keeps its blocks from one that holds no blocks of its footprint";
        return false;
    }
    be->barrier();
    if (!owner->_keptBlocks->_raw.wc)
        rc->clean(owner->_keptBlocks->_raw.cpu + at, k.planeBytes * planes, true);
    // then every form of the destination's family decoded from its kept copy
    const uint32_t region[3] = {origin[0], origin[1], volume ? origin[2] : 0};
    return rmEncodeFamilyUpload(rc, cb, be, owner->_keptBlocks, at, k.rowBytes, k.planeBytes, owner, size,
                                volume ? 0 : t->_slicesBase + slice, t->_levelsBase + level, region, false, why);
}

bool rmDecodedGetBytes(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, void *bytes, NSUInteger row,
                       NSUInteger image)
{
    RMTexture *owner = rmFamilyOwner(t);
    if (!owner)
        return rmDecodedReadback(t, "getBytes");
    const raw::DecodedFormat &f = *rmDecoded(owner);
    level += t->_levelsBase;
    slice += t->_slicesBase;
    if (level >= owner->_descriptor.mipmapLevelCount || r.origin.x % f.blockW || r.origin.y % f.blockH)
        return true;
    const MTLSize at = levelSize((id<MTLTexture>)owner, level);
    if (r.origin.x >= at.width || r.origin.y >= at.height)
        return true;
    const raw::KeptLevel k = keptLevelOf(owner, uint32_t(level));
    const uint32_t across = raw::decodedBlocks(uint32_t(std::min(r.size.width, at.width - r.origin.x)), f.blockW),
                   down = raw::decodedBlocks(uint32_t(std::min(r.size.height, at.height - r.origin.y)), f.blockH);
    const bool volume = owner->_raw.type == raw::fx::kTexThreeD;
    const uint64_t line = uint64_t(across) * f.blockBytes, to = row ? row : line, toPlane = image ? image : to * down;
    raw::HostAccess access;
    if (!gRM->raw->hostAccess(owner->_keptBlocks->_raw, false, access))
        return true;
    for (NSUInteger p = 0; p < (volume ? r.size.depth : 1); p++) {
        const uint64_t z = volume ? r.origin.z + p : slice;
        for (uint32_t y = 0; y < down; y++)
            memcpy(static_cast<uint8_t *>(bytes) + p * toPlane + y * to,
                   owner->_keptBlocks->_raw.cpu + k.offset + z * k.planeBytes +
                       uint64_t(r.origin.y / f.blockH + y) * k.rowBytes +
                       uint64_t(r.origin.x / f.blockW) * f.blockBytes,
                   line);
    }
    return true;
}
