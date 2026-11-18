// SPDX-License-Identifier: MIT
#import "RawMetalXrPlanes.h"
#include "raw_format_standin.h"
#include "raw_surface_refusal.h"
#include "raw_xr_pools.h"

#include <cstring>

static_assert(kXrPlanesOverflowTag >= raw::kFormatStandInTag + 2 * 8, "an overflow image's tag shares a fixed tag's memory");

bool xrPlanesStorable(MTLTextureDescriptor *d)
{
    const bool stores = d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageShaderWrite);
    return !stores || (gRM->raw->xrPlanesImagePipeline(false) && gRM->raw->xrPlanesImagePipeline(true));
}

// A 2D texture of one level and sample, not turned
static bool planesForm(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return raw::twoPlaneXr(uint32_t(d.pixelFormat)) && xrPlanesStorable(d) && d.textureType == MTLTextureType2D &&
           d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned;
}

bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    return planesForm(d) && plane == 0 && IOSurfaceGetPlaneCount(s) == 2 &&
           IOSurfaceGetBytesPerRowOfPlane(s, 0) >= 4 * d.width && IOSurfaceGetHeightOfPlane(s, 0) >= d.height &&
           IOSurfaceGetBytesPerRowOfPlane(s, 1) >= d.width && IOSurfaceGetHeightOfPlane(s, 1) >= d.height;
}

bool xrWordPlaneShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    const size_t planes = IOSurfaceGetPlaneCount(s);
    const bool writes =
        d.usage == MTLTextureUsageUnknown || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
    return planesForm(d) && writes && (planes <= 1 ? !plane : plane && plane < planes);
}

static bool holdInPlanes(RMTexture *t, IOSurfaceRef s, uint64_t word, uint32_t wordPitch, uint64_t alpha,
                         uint32_t alphaPitch, uint32_t width, uint32_t height)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const char *why = "";
    if (!gRM->raw->createXrPlaneTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s,
                                        word, wordPitch, alpha, alphaPitch, width, height, uint32_t(d.pixelFormat),
                                        t->_raw, &why)) {
        raw::surfaceRefusal = why;
        return false;
    }
    if (!keepExtendedRange(d, t->_raw, false)) {
        raw::surfaceRefusal = "no memory or texture header for the stand-in it is sampled through";
        gRM->raw->destroyTexture(t->_raw);
        return false;
    }
    return true;
}

bool makeXrWordPlane(RMTexture *t, IOSurfaceRef s, uint32_t width, uint32_t height)
{
    const bool planes = IOSurfaceGetPlaneCount(s) > 0;
    const uint64_t word =
        planes ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, t->_plane)) - uintptr_t(IOSurfaceGetBaseAddress(s)) : 0;
    const size_t pitch = planes ? IOSurfaceGetBytesPerRowOfPlane(s, t->_plane) : IOSurfaceGetBytesPerRow(s);
    return holdInPlanes(t, s, word, uint32_t(pitch), 0, 0, width, height);
}

bool makeXrPlanes(RMTexture *t, IOSurfaceRef s, uint32_t width, uint32_t height)
{
    const uintptr_t at = uintptr_t(IOSurfaceGetBaseAddress(s));
    MTLTextureDescriptor *d = t->_descriptor;
    const uint64_t word = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
                   alpha = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at;
    if (!holdInPlanes(t, s, word, uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)), alpha,
                      uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1)), width, height))
        return false;
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        const raw::Texture &w = *t->_raw.luma, &a = *t->_raw.chroma;
        os_log(rawLog(),
               "raw: two-plane XR on (%ux%u '%c%c%c%c', drawn in its planes: rows %u and %u, va&127 %llu and %llu, %s "
               "and %s)",
               unsigned(d.width), unsigned(d.height), char(IOSurfaceGetPixelFormat(s) >> 24),
               char(IOSurfaceGetPixelFormat(s) >> 16), char(IOSurfaceGetPixelFormat(s) >> 8),
               char(IOSurfaceGetPixelFormat(s)), w.pitch, a.pitch, (unsigned long long)(w.va & 127),
               (unsigned long long)(a.va & 127), w.renderable ? "direct" : "shadowed",
               a.renderable ? "direct" : "shadowed");
    }
    return true;
}

uint32_t xrWordsFormat(uint32_t format)
{
    constexpr uint32_t kBgr10Xr = 554, kBgr10XrSrgb = 555;
    return format == raw::kRgb10A8XrSrgb ? kBgr10XrSrgb : kBgr10Xr;
}

bool xrWordsTexture(RMTexture *t)
{
    return t->_surface && t->_rawOk && raw::twoPlaneXr(uint32_t(t->_descriptor.pixelFormat)) && !t->_raw.xrPlanes;
}

void readAlphaZero(raw::Texture &standIn)
{
    constexpr uint8_t kNoAlpha[4] = {2, 3, 4, 0};  // MTLTextureSwizzle Red, Green, Blue, Zero
    gRM->raw->swizzle(standIn, kNoAlpha);
}

bool xrPlaneWords(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
                  bool write)
{
    const raw::Texture *w = t->_rawOk && t->_raw.xrPlanes ? t->_raw.luma : nullptr;
    if (!w || !w->cpu || level || slice || r.origin.z || r.size.depth != 1 || r.origin.x + r.size.width > w->width ||
        r.origin.y + r.size.height > w->height)
        return false;
    const size_t line = r.size.width * raw::bufferTexelBytes(t->_raw.format, t->_raw.bpp);
    uint8_t *first = w->cpu + r.origin.y * w->pitch + r.origin.x * w->bpp;
    raw::HostAccess access;
    if (!gRM->raw->hostAccess(*w, first, (r.size.height - 1) * w->pitch + line, write, access))
        [NSException raise:NSInternalInconsistencyException format:@"GPU texture CPU ownership failed"];
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *words = first + y * w->pitch;
        if (write)
            memcpy(words, bytes + y * row, line);
        else
            memcpy(bytes + y * row, words, line);
    }
    return true;
}

static int planesPass(raw::CommandBuffer *rc, const raw::PassDesc &p, raw::Pipeline *state, const raw::Texture &read0,
                      const raw::Texture *read1, uint32_t width, uint32_t height, const char **why)
{
    raw::RenderEncoder *e = rc->render(p);
    if (!e) {
        *why = "a two-plane RGB10A8_2P_XR10 texture's image or planes the raw path cannot render";
        return 1;
    }
    e->setPipeline(state);
    e->setTexture(0, &read0);
    if (read1)
        e->setTexture(1, read1);
    e->setViewport(0, 0, width, height, 0, 1);
    e->setScissor(0, 0, width, height);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return 2;
    e->end();
    return 0;
}

int mergeXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, uint32_t k, raw::Texture *&image, const char **why)
{
    constexpr uint32_t kRG32Uint = 103;
    image = rc->turnScratch(kRG32Uint, t.width, t.height, xrPlanesImageTag(k));
    raw::Pipeline *merge = gRM->raw->xrPlanesImagePipeline(false);
    if (!image || !merge) {
        *why = !image ? "no image for a written two-plane RGB10A8_2P_XR10 texture"
                      : "the pack has no state merging a two-plane RGB10A8_2P_XR10 texture's planes";
        return 1;
    }
    raw::fillXrPools(*rc, t);
    raw::PassDesc p;
    p.color[0].texture = image;
    p.color[0].load = raw::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    return planesPass(rc, p, merge, *t.luma, t.chroma, t.width, t.height, why);
}

int splitXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, const raw::Texture &image, const char **why)
{
    raw::Pipeline *split = gRM->raw->xrPlanesImagePipeline(true);
    if (!split) {
        *why = "the pack has no state splitting a two-plane RGB10A8_2P_XR10 texture's image";
        return 1;
    }
    raw::PassDesc p;
    p.color[0].texture = t.luma;
    p.color[1].texture = t.chroma;
    for (raw::ColorAttachment &a : p.color) {
        a.load = raw::ColorAttachment::DontCare;
        a.store = true;
    }
    p.count = 2;
    return planesPass(rc, p, split, image, nullptr, t.width, t.height, why);
}
