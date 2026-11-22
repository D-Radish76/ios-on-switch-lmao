// SPDX-License-Identifier: MIT
// A blit encoder's commands of the tape onto the copy engine (and the 2D engine's fills and mip chains): see encodeRaw.
#import "RawMetalInternal.h"
#import "RawMetalBlitImages.h"
#import "RawMetalBlitRules.h"
#import "RawMetalCopyClamp.h"
#import "RawMetalDecoded.h"
#import "RawMetalStandIns.h"
#import "RawMetalXrCopies.h"
#include "raw_format_standin.h"
#include "../fx/fx_mip_probe.h"
#include "../fx/fx_mip_route.h"
#include "raw_attachment_target.h"
#include "raw_blit_planes.h"
#include "raw_fill_record.h"
#include "raw_mip.h"
#include "raw_overlap_rows.h"
#include "raw_rotated_pass.h"
#include "raw_twod_fill.h"

// What stands for a texture's memory in its blit encoder's order (raw::BlitEncoder::access): its surface, its buffer, or the
// texture a view views
static const void *memoryOf(RMTexture *t)
{
    if (t && t->_levelsOwner)
        t = t->_levelsOwner;
    if (t && t->_surface)
        return t->_surface;
    if (t && t->_buffer)
        return (__bridge const void *)t->_buffer;
    return (__bridge const void *)t;
}

static const void *memoryOf(RMBuffer *b)
{
    return (__bridge const void *)b;
}

// The copy engine's copies of those rows into or out of plane z: one copy of every row where they lie a pitch apart, else one
// per row
static void copyRows(raw::BlitEncoder *be, uint64_t bufferVa, const BufferRows &r, uint64_t offset,
                     uint64_t bytesPerRow, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                     uint32_t h, bool intoTexture, uint8_t aspect)
{
    if (r.uniform) {
        be->copyBufferTexels(bufferVa + r.first, uint32_t(r.pitch), t, x, y, z, w, h, intoTexture, aspect);
        return;
    }
    for (uint32_t k = 0; k < h; k++)
        be->copyBufferTexels(bufferVa + bufferRow(r, offset, bytesPerRow, k), w * raw::bufferUnit(t, aspect), t, x,
                             y + k, z, w, 1, intoTexture, aspect);
}

// A copy side's z (raw::BlitEncoder): an array's or cube's layer, else a 3D texture's plane
static uint32_t layerOrPlane(const raw::Texture &t, uint32_t slice, uint32_t z)
{
    return t.layers > 1 ? slice : z;
}

// A buffer<->texture copy of `planes` planes from plane z, `image` bytes apart in the buffer (each plane's rows decided at
// record: bufferPlanes); the buffer's lines cleaned before, or invalidated for the CPU after where the copy writes it
static void copyPlanes(raw::BlitEncoder *be, raw::CommandBuffer *rc, const raw::Buffer &b, uint64_t offset,
                       uint64_t row, uint64_t image, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t z,
                       uint32_t w, uint32_t h, uint32_t planes, bool intoTexture, uint8_t aspect)
{
    raw::toElements(t, x, y, w, h);  // (whole blocks: checked at record)
    const uint32_t unit = raw::bufferUnit(t, aspect);
    for (uint32_t k = 0; k < std::max(planes, 1u); k++) {
        const uint64_t at = offset + k * image;
        BufferRows rows;
        bufferRows(at, row, h, w * unit, unit, intoTexture, rows);
        const uint64_t first = bufferRow(rows, at, row, 0);
        if (!b.wc)
            rc->clean(b.cpu + first, bufferRow(rows, at, row, h - 1) - first + w * unit, !intoTexture);
        copyRows(be, b.va, rows, at, row, t, x, y, z + k, w, h, intoTexture, aspect);
    }
}

// A no-copy texture copied into a pool image (raw::Texture::callerPages): what a copy wrote into the image goes on to the
// caller's memory, which stays the texture's contents, after it on the copy engine
static void copyToCallerPages(raw::BlitEncoder *be, raw::CommandBuffer *rc, RMTexture *t)
{
    t = t->_levelsOwner ?: t;
    const raw::Texture &x = t->_raw;
    if (!x.callerPages)
        return;
    be->access(memoryOf(t), x.callerPages);
    rc->use(x.callerPages, true);
    rc->clean(x.callerPages->bo.cpu, x.storage.bytes, true);
    be->copyBytes(x.imagePitchVa, x.callerPages->bo.va, x.storage.bytes);
}

// The image a buffer copy moves of `t`'s level: a texture held through a surface's planes moves plane 0's bytes, at the
// rows its writes land at `into` it (raw_blit_planes.h; RGB10A8_2P_XR10's colour words: BlitEncoder::copyBufferTexels)
static void bufferCopyImage(RMTexture *t, uint32_t level, raw::Texture &image, bool into)
{
    blitImage(t, level, image);
    if (t->_raw.converted())
        raw::planeBytes(t->_raw, raw::planeUnit(t->_raw), image, into);
    else if (into && image.storePitch)
        image = raw::storedRows(image, image);
}

// Plane 0's bytes of the side or sides held through planes, counted in the destination's texel (kCopyPlaneBytes): the
// copy engine's lines `pitch` apart, each its texels' bytes in one run, which past a row's end are the next rows' (a
// source's lines may overlap); a plain source of another texel size read in it (raw_blit_planes.h imageInUnits)
static void copyPlaneBytes(raw::BlitEncoder *be, RMTexture *s, uint32_t sl, uint32_t ss, uint32_t sx, uint32_t sy,
                           uint32_t sz, RMTexture *t, uint32_t dl, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t dz,
                           uint32_t w, uint32_t h)
{
    const bool fromPlanes = raw::heldInPlanes(s->_raw), intoPlanes = raw::heldInPlanes(t->_raw);
    raw::Texture from, to;
    if (intoPlanes)
        raw::planeBytes(t->_raw, raw::planeUnit(t->_raw), to, true);
    else
        blitImage(t, dl, to);
    raw::Texture image;
    if (fromPlanes)
        raw::planeBytesRead(s->_raw, to.bpp, sx, sy, w, h, from);
    else if (blitImage(s, sl, image) && image.bpp != to.bpp)
        raw::imageInUnits(image, to.bpp, from);
    else
        from = image;
    be->copyTexels(from, sx, sy, fromPlanes ? 0 : layerOrPlane(from, ss, sz), to, dx, dy,
                   intoPlanes ? 0 : layerOrPlane(to, ds, dz), w, h);
}

// Two textures held through planes (kCopyPlanes): plane 0's bytes in the destination's unit, and plane 1 a byte a texel
// at each side's plane-0 rows on through its end (raw_blit_planes.h planeOneRows), zeros past the source's last page,
// nothing past the destination's; a source of no plane 1 leaves the destination's as it is
static void copyPlanePair(raw::BlitEncoder *be, const raw::Texture &s, uint32_t sx, uint32_t sy, const raw::Texture &d,
                          uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    const uint32_t unit = raw::planeUnit(d);
    raw::Texture from, to;
    if (!raw::planeBytesRead(s, unit, sx, sy, w, h, from) || !raw::planeBytes(d, unit, to, true))
        return;
    be->copyTexels(from, sx, sy, 0, to, dx, dy, 0, w, h);
    raw::Texture fromOne, toOne;
    if (!raw::planeOneRows(d, to.pitch, toOne) || !raw::planeOneRows(s, from.pitch, fromOne))
        return;
    constexpr uint8_t kZero[16] = {};
    raw::planeOneRuns(fromOne.storage.bytes, fromOne.pitch, sx, sy, toOne.storage.bytes, toOne.pitch, dx, dy, w, h,
                      [&](const raw::PlaneOneRun &r) {
                          if (r.copied)
                              be->copyTexels(fromOne, sx, sy + r.row, 0, toOne, dx, dy + r.row, 0, r.copied, r.rows);
                          if (r.zeroed)
                              be->fillRect(toOne, dx + r.copied, dy + r.row, 0, r.zeroed, r.rows, kZero, 1, 0);
                      });
}

// A fill's units into each image of `image` (raw_fill_record.h): a turned plane's region turned as Metal stores it, a
// multisampled one's every sample, a 3D level's planes; the 2D engine's solid rectangle where rule clear.twod holds for the
// format and memory (raw_twod_fill.h), else the copy engine's constants
static void fillImage(raw::BlitEncoder *be, raw::Texture image, const raw::FillRecord &f)
{
    uint32_t x0 = f.x0, y0 = f.y0, x1 = f.x1, y1 = f.y1;
    if (image.rotation) {
        const raw::Rect turned = raw::frameOf(image).rect({x0, y0, x1, y1});
        x0 = turned.x0, y0 = turned.y0, x1 = turned.x1, y1 = turned.y1;
    }
    if (image.samples > 1) {
        uint32_t gx, gy;
        raw::sampleGrid(image.samples, gx, gy);
        x0 <<= gx, x1 <<= gx, y0 <<= gy, y1 <<= gy;
        image.width <<= gx;
        image.height <<= gy;
        image.samples = 1;
    } else if (raw::twodFillHolds(image.format, image.comptags) && !f.words && f.bytes == image.bpp &&
               image.type != raw::fx::kTexThreeD && (image.blockLinear || image.renderable) && !image.zeta) {
        raw::Texture target = image;
        target.va += uint64_t(f.layer) * target.layerBytes;
        target.layers = 1;
        target.type = raw::fx::kTexTwoD;
        target.tic = 0;
        target.renderable = true;
        // a crop of the kernel's tiled surface, as raw::sideOf places it
        if (image.import && image.import->kernelTiled()) {
            x0 += image.surfaceX, x1 += image.surfaceX, y0 += image.surfaceY, y1 += image.surfaceY;
            target.width = image.pitch / image.bpp;
            target.height = image.import->layout.height;
        }
        uint32_t word = 0;
        memcpy(&word, f.texel, std::min<uint32_t>(f.bytes, sizeof word));
        be->fillTwod(target, x0, y0, x1, y1, word);
        RAW_DIAGNOSTIC(gRM->raw->stats.fillTwod++);
        return;
    }
    for (uint32_t z = f.z0; z < f.z1; z++)
        be->fillRect(image, x0, y0, layerOrPlane(image, f.layer, z), x1 - x0, y1 - y0, f.texel, f.bytes, f.words);
}

bool encodeBlit(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc, raw::BlitEncoder *&be,
                std::set<RMTexture *> &fresh, RMStandInFresh &standInFresh, const char **why)
{
    switch (c.op) {
    case OpCopyTexture: {
        RMTexture *s = (__bridge RMTexture *)c.obj, *t = (__bridge RMTexture *)c.obj2;
        useTexture(rc, s, false);
        useTexture(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        const uint64_t x = c.x, o = extra(cb, x + 10), sl = extra(cb, x + 1), dl = extra(cb, x + 9);
        const uint32_t sx = uint32_t(extra(cb, x + 2)), sy = uint32_t(extra(cb, x + 3)),
                       sz = uint32_t(extra(cb, x + 4)), w = uint32_t(extra(cb, x + 5)), h = uint32_t(extra(cb, x + 6)),
                       d = uint32_t(extra(cb, x + 7)), dx = uint32_t(o), dy = uint32_t(o >> 32),
                       dz = uint32_t(extra(cb, x + 11)), ss = uint32_t(extra(cb, x)), ds = uint32_t(extra(cb, x + 8));
        if (c.a == kCopyXrPlanes) {
            copyXrPlanes(be, s, ss, sx, sy, t, ds, dx, dy, w, h);
            break;
        }
        if (c.a == kCopyPlanes) {
            copyPlanePair(be, s->_raw, sx, sy, t->_raw, dx, dy, w, h);
            break;
        }
        if (c.a == kCopyPlaneBytes) {
            copyPlaneBytes(be, s, uint32_t(sl), ss, sx, sy, sz, t, uint32_t(dl), ds, dx, dy, dz, w, h);
            if (!raw::heldInPlanes(t->_raw))
                copyToCallerPages(be, rc, t);
            break;
        }
        raw::Texture fromLevel, toLevel;
        blitImage(s, uint32_t(sl), fromLevel);
        blitImage(t, uint32_t(dl), toLevel);
        if (toLevel.storePitch)  // rows under 16 bytes, which an M4 Max's blit writes 16 apart (raw_plain_rows.h)
            toLevel = raw::storedRows(toLevel, toLevel);
        uint32_t ex = sx, ey = sy, ew = w, eh = h;
        raw::toElements(s->_raw, ex, ey, ew, eh);  // (whole blocks, the same blocks both sides: checked at record)
        uint32_t fx = dx / t->_raw.blockW, fy = dy / t->_raw.blockH;
        if (fromLevel.samples > 1) {
            uint32_t gx, gy;
            raw::sampleGrid(fromLevel.samples, gx, gy);
            for (raw::Texture *grid : {&fromLevel, &toLevel}) {
                grid->width <<= gx;
                grid->height <<= gy;
                grid->samples = 1;
            }
            ex <<= gx;
            ey <<= gy;
            ew <<= gx;
            eh <<= gy;
            fx <<= gx;
            fy <<= gy;
        }
        const bool out = c.a == kCopyStencilOut;
        const raw::Texture &zeta = out ? fromLevel : toLevel, &bytes = out ? toLevel : fromLevel;
        for (uint32_t k = 0; k < std::max(d, 1u); k++) {
            const uint32_t from = layerOrPlane(fromLevel, ss, sz + k), to = layerOrPlane(t->_raw, ds, dz + k);
            if (out || c.a == kCopyStencilIn)
                be->copyStencilTexels(zeta, out ? ex : fx, out ? ey : fy, out ? from : to, bytes, out ? fx : ex,
                                      out ? fy : ey, out ? to : from, ew, eh, !out);
            else
                be->copyTexels(fromLevel, ex, ey, from, toLevel, fx, fy, to, ew, eh, uint8_t(c.a));
        }
        if (rmCopyClamps(s, uint32_t(sl), t, uint32_t(dl)) &&
            !rmClampCopiedTexture(rc, be, t, uint32_t(dl), ds, dz, d, dx, dy, w, h, why))
            return false;
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyBuffer: {
        RMBuffer *s = (__bridge RMBuffer *)c.obj, *t = (__bridge RMBuffer *)c.obj2;
        useBuffer(rc, s, false);
        useBuffer(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t n = extra(cb, c.x + 1), dof = extra(cb, c.x);
        if (!s->_raw.wc)
            rc->clean(s->_raw.cpu + c.c, n);
        if (!t->_raw.wc)
            rc->clean(t->_raw.cpu + dof, n, true);
        be->copyBytes(s->_raw.va + c.c, t->_raw.va + dof, n);
        break;
    }
    case OpFillBuffer: {
        RMBuffer *t = (__bridge RMBuffer *)c.obj;
        const uint64_t n = extra(cb, c.x);
        useBuffer(rc, t, true);
        be->access(nullptr, memoryOf(t));
        if (!t->_raw.wc)
            rc->clean(t->_raw.cpu + c.c, n, true);
        be->fillBytes(t->_raw.va + c.c, n, uint8_t(c.a));
        break;
    }
    case OpFillTexture: {
        RMTexture *t = (__bridge RMTexture *)c.obj;
        useTexture(rc, t, true);
        be->access(nullptr, memoryOf(t));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        uint64_t words[raw::kFillRecordWords];
        for (uint32_t k = 0; k < raw::kFillRecordWords; k++)
            words[k] = extra(cb, c.x + raw::kFillCallWords + k);
        raw::Texture image;
        blitImage(t, uint32_t(c.c), image);
        fillImage(be, image, raw::unpackFill(words));
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyBufferToTexture: {
        RMBuffer *s = (__bridge RMBuffer *)c.obj;
        RMTexture *t = (__bridge RMTexture *)c.obj2;
        useBuffer(rc, s, false);
        useTexture(rc, t, true);
        if (rmDecoded(t)) {
            const uint64_t x = c.x, o = extra(cb, x + 8);
            const uint32_t size[3] = {uint32_t(extra(cb, x + 3)), uint32_t(extra(cb, x + 4)),
                                      uint32_t(extra(cb, x + 5))};
            const uint32_t origin[3] = {uint32_t(o), uint32_t(o >> 32), uint32_t(extra(cb, x + 9))};
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            be->end();
            const bool decoded =
                rmEncodeDecodedCopy(rc, s, t, extra(cb, x), extra(cb, x + 1), extra(cb, x + 2), size,
                                    uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 7)), origin, why);
            be = rc->blit();
            if (!decoded)
                return false;
            break;
        }
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t x = c.x, o = extra(cb, x + 8), so = extra(cb, x), row = extra(cb, x + 1),
                       image = extra(cb, x + 2);
        const uint64_t w = extra(cb, x + 3), h = extra(cb, x + 4), d = extra(cb, x + 5), dl = extra(cb, x + 7),
                       dz = layerOrPlane(t->_raw, uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 9)));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        raw::Texture level;
        bufferCopyImage(t, uint32_t(dl), level, true);
        copyPlanes(be, rc, s->_raw, so, row, image, level, uint32_t(o), uint32_t(o >> 32), uint32_t(dz), uint32_t(w),
                   uint32_t(h), uint32_t(d), true, uint8_t(c.a));
        if (rmCopyClamps(nil, 0, t, uint32_t(dl)) &&
            !rmClampCopiedTexture(rc, be, t, uint32_t(dl), uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 9)),
                                  uint32_t(d), uint32_t(o), uint32_t(o >> 32), uint32_t(w), uint32_t(h), why))
            return false;
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyTextureToBuffer: {
        RMTexture *s = (__bridge RMTexture *)c.obj;
        RMBuffer *t = (__bridge RMBuffer *)c.obj2;
        useTexture(rc, s, false);
        useBuffer(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t x = c.x, o = extra(cb, x + 2),
                       sz = layerOrPlane(s->_raw, uint32_t(extra(cb, x)), uint32_t(extra(cb, x + 3))),
                       w = extra(cb, x + 4), h = extra(cb, x + 5), d = extra(cb, x + 6), dof = extra(cb, x + 7),
                       row = extra(cb, x + 8), image = extra(cb, x + 9), sl = extra(cb, x + 1);
        raw::Texture level;
        bufferCopyImage(s, uint32_t(sl), level, false);
        copyPlanes(be, rc, t->_raw, dof, row, image, level, uint32_t(o), uint32_t(o >> 32), uint32_t(sz), uint32_t(w),
                   uint32_t(h), uint32_t(d), false, uint8_t(c.a));
        if (rmCopyClamps(s, uint32_t(sl), nil, 0) &&
            !rmClampCopiedBuffer(rc, be, t, dof, row, image, uint32_t(w), uint32_t(h), uint32_t(d), why))
            return false;
        break;
    }
    default:
        break;
    }
    return true;
}

bool rmFormatMips(uint32_t format)
{
    const char *state = raw::formatMipState(format);
    return state && gRM->raw->ownPipeline(state);
}

// Levels first + 1 .. last of a view's layers by the pack's mip state (raw_mip.h, raw_mip_passes.cpp): a format whose
// words the GM20B cannot filter by its word mip state, one the GM20B renders none of by its own (raw_format_standin.h)
bool mipPasses(raw::CommandBuffer *rc, RMTexture *t, uint32_t first, uint32_t last, const char **why)
{
    __unsafe_unretained RMTexture *owner = t->_levelsOwner ?: t;
    const uint32_t format = t->_raw.format, firstLayer = t->_slicesBase, layers = t->_raw.layers;
    if (raw::wordMipState(format)) {
        useTexture(rc, t, true);
        return raw::wordMipPasses(*gRM->raw, *rc, owner->_raw, first, last, firstLayer, layers, format, why);
    }
    if (raw::formatStandIn(t->_raw.format)) {
        useTexture(rc, t, true);
        return raw::formatMipPasses(*gRM->raw, *rc, owner->_raw, first, last, firstLayer, layers, format, why);
    }
    raw::Pipeline *mip = gRM->raw->mipPipeline(t->_raw.format);
    if (!mip)
        return false;
    useTexture(rc, t, true);
    if (!rc->mipPasses(owner->_raw, *mip, first, last, firstLayer, layers, format, why))
        return false;
    RAW_DIAGNOSTIC(gRM->raw->stats.mip3dLevels += (last - first) * layers);
    return true;
}

// Levels first + 1 .. last of a 3D texture's chain by the pack's volume mip state (raw_mip.h mip3dState): one pass a plane of
// each level, drawn from a volume header of the level above, its step the two levels' sizes, depths and the plane; a format
// with no such state by its word mip state
static bool mipVolumePasses(raw::CommandBuffer *rc, RMTexture *t, uint32_t first, uint32_t last, const char **why)
{
    raw::Device &d = *gRM->raw;
    __unsafe_unretained RMTexture *owner = t->_levelsOwner ?: t;
    raw::Pipeline *mip = d.mip3dPipeline(t->_raw.format);
    const raw::fx::MipChain &c = owner->_raw.chain->layout;
    if (!mip && raw::wordMip3dState(t->_raw.format)) {
        useTexture(rc, t, true);
        return raw::wordMipPasses(d, *rc, owner->_raw, first, last, 0, 1, t->_raw.format, why);
    }
    if (!mip || last >= c.levels)
        return false;
    const raw::FormatInfo *f = raw::formatInfo(t->_raw.format);
    useTexture(rc, t, true);
    for (uint32_t l = first; l < last; l++) {
        raw::Texture from = owner->_raw;
        if (!(from.tic = d.levelTic(owner->_raw, l, f && f->srgb)))
            return false;
        if (from.format != t->_raw.format) {
            from.format = t->_raw.format;
            if (!(from.tic = d.viewTic(from, f->tic0)))
                return false;
        }
        const raw::fx::MipLevel &above = c.level[l], &level = c.level[l + 1];
        for (uint32_t z = 0; z < level.depth; z++) {
            raw::Texture to;
            if (!raw::attachmentTarget(owner->_raw, l + 1, z, to))
                return false;
            to.format = t->_raw.format;
            to.renderable = f->rt != 0;
            to.layers = 1;
            raw::PassDesc p;
            p.color[0].texture = &to;
            p.color[0].load = raw::ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            p.width = level.width;
            p.height = level.height;
            raw::RenderEncoder *e = rc->render(p);
            if (!e)
                return false;
            e->setPipeline(mip);
            e->setTexture(0, &from);
            const uint32_t step[8] = {above.width, above.height, level.width, level.height,
                                      above.depth, level.depth,  z,           0};
            e->setBytes(2, 0, step, sizeof step);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return false;
            e->end();
        }
    }
    RAW_DIAGNOSTIC(d.stats.mip3dLevels += last - first);
    return true;
}

// generateMipmapsForTexture: each run of the chain's steps on its engine (fx_mip_route.h, decided at record), the blit
// encoder ended around 3D runs and `be` the one encoding goes on with. `probe2d`: a debug build's probe of a format the pack's
// mip state also makes, every step on the 2D engine as that surface format (fx_mip_probe.h)
bool encodeMipmaps(raw::CommandBuffer *rc, RMTexture *t, uint32_t probe2d, raw::BlitEncoder *&be, const char **why)
{
    __unsafe_unretained RMTexture *owner = t->_levelsOwner ?: t;
    const raw::Texture::Chain &chain = *owner->_raw.chain;
    const uint32_t first = t->_levelsBase, last = first + uint32_t(t->_descriptor.mipmapLevelCount) - 1;
    if (t->_raw.type == raw::fx::kTexThreeD) {
        be->end();
        const bool ok = mipVolumePasses(rc, t, first, last, why);
        be = rc->blit();
        return ok;
    }
    const uint32_t fmt2d = probe2d                                ? probe2d
                           : t->_raw.format == owner->_raw.format ? chain.fmt2d
                                                                  : raw::fx::twodChainFormat(t->_raw.format);
    raw::Device &d = *gRM->raw;
    bool ok = true;
    const bool routed = raw::fx::eachMipRun(
        chain.layout, first, last, fmt2d != 0,
        !probe2d && (d.mipPipeline(t->_raw.format) || rmFormatMips(t->_raw.format) ||
                     d.ownPipeline(raw::wordMipState(t->_raw.format))),
        [&](const raw::fx::MipRun &run) {
            if (!ok)
                return;
            if (run.engine == raw::fx::MipEngine::TwoD) {
                useTexture(rc, t, true);
                be->access(memoryOf(t), memoryOf(t));
                be->generate(owner->_raw, fmt2d, run.first, run.last, run.unproven, t->_slicesBase, t->_raw.layers);
                return;
            }
            be->end();
            ok = mipPasses(rc, t, run.first, run.last, why);
            be = rc->blit();
        });
    return routed && ok;
}
