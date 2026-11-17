// SPDX-License-Identifier: MIT
// See raw_gpu.h (textures over surfaces, buffers and YCbCr planes).
#include "raw_lock.h"
#include "raw_gpu.h"
#include "raw_surface_view.h"
#include "TegraGM20BLayout.h"

#include <algorithm>
#include <cstring>

#include "raw_gpu_internal.h"

namespace raw {

// A texture over a block-linear surface's import: TegraGM20BLayout.h's image from the surface's first byte, at the shape the
// kernel decided (a view of another format of the same texel size reads it as its own), under the import's
// TGM_LAYOUT_KIND binding, which the 3D and 2D engines, the copy engine and the texture unit all address so
bool Device::tiledSurfaceTexture(uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, Texture &t,
                                 Texture *sampled)
{
    const tgm_layout &l = t.import->layout;
    SurfaceView view;
    if (!surfaceView(l, offset, width, height, t.bpp, pitch, view) || (!view.direct && !sampled)) {
        release(t.import);
        t.import = nullptr;
        return false;
    }
    t.va = t.import->bo.va;
    t.cpu = reinterpret_cast<uint8_t *>(t.import->base);
    t.pitch = l.row_bytes;
    t.blockLinear = true;
    t.blockHeight = l.y_log2;
    t.layerBytes = l.bytes;
    t.storage = {t.va, l.bytes, !t.import->id};
    t.surfaceX = view.x;
    t.surfaceY = view.y;
    t.surfaceView = !view.direct;
    t.shadowTarget = view.x || view.y;
    if (t.surfaceView) {
        t.sampled = sampled;
        t.tic = sampled->tic;
        return true;
    }
    if (!(t.tic = allocTic())) {
        release(t.import);
        t.import = nullptr;
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::createSurfaceTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                  uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format,
                                  Texture &t, bool writes, uint64_t readable)
{
    const uint64_t page = pageSize(),
                   mapped = ((uintptr_t(base) + allocBytes + page - 1) & ~(page - 1)) - uintptr_t(base);
    const bool readsPast = readable != 0;
    readable = readsPast ? std::min(readable, mapped) : allocBytes;
    // rows and offset a multiple of 16, as m2v takes a surface (linear_plan.rs); the texture header's 32 through `sampled`.
    // A plane read past its rows' ends (readsPast: a YCbCr plane read with more codes a row than its pitch holds) has rows
    // that overlap, which no header lays out: the copy engine gathers them into `sampled` too. A base off a page (a surface
    // over the client's memory) is imported from its page, its VA as far into it (bindImport).
    const bool offPage = uintptr_t(base) & (page - 1);
    if (!shape(width, height, format, t) || (pitch & 15) || ((uintptr_t(base) + offset) & 15) ||
        (pitch < width * t.bpp && !readsPast) || offset > readable ||
        uint64_t(pitch) * (height - 1) + uint64_t(width) * t.bpp > readable - offset)
        return false;
    const bool unaligned = ((pitch | (uintptr_t(base) + offset)) & 31) || pitch < width * t.bpp;
    Texture *sampled = nullptr;
    if (unaligned) {
        sampled = new Texture;
        if (!createTexture(width, height, format, *sampled, false, true)) {
            delete sampled;
            return false;
        }
    }
    std::unique_lock<raw::Mutex> l(lock_);
    t.pitch = pitch;
    const LayoutAsk ask = {width, height, t.bpp, pitch, writes && !offset && !sampled};
    t.import = importSurface(surfaceId, base, (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1), surface, &ask);
    if (t.import && offPage && t.import->kernelTiled()) {
        release(t.import);
        t.import = nullptr;
    }
    if (t.import && t.import->kernelTiled()) {
        SurfaceView view;
        const bool valid = surfaceView(t.import->layout, offset, width, height, t.bpp, pitch, view);
        if (valid && !view.direct && !sampled) {
            l.unlock();
            sampled = new Texture;
            if (!createTexture(width, height, format, *sampled, false, true)) {
                delete sampled;
                sampled = nullptr;
            }
            l.lock();
        }
        tiledSurfaceTexture(offset, width, height, pitch, t, sampled);
    }
    if (!t.import) {
        l.unlock();
        if (sampled && sampled != t.sampled) {
            destroyTexture(*sampled);
            delete sampled;
        }
        return false;
    }
    if (t.blockLinear) {
        l.unlock();
        if (sampled && sampled != t.sampled) {
            destroyTexture(*sampled);
            delete sampled;
        }
        return true;
    }
    t.va = t.import->bo.va + offset;
    const uint64_t extent =
        std::max(uint64_t(pitch) * height, uint64_t(pitch) * (height - 1) + uint64_t(width) * t.bpp);
    t.storage = {t.va, std::min(extent, readable - offset), !t.import->id};
    t.cpu = static_cast<uint8_t *>(base) + offset;
    t.shadowTarget = t.renderable && !linearTarget(t);
    t.renderable = t.renderable && linearTarget(t);
    if (sampled) {  // sampled through its pool image's header
        t.sampled = sampled;
        t.tic = sampled->tic;
        return true;
    }
    if (!(t.tic = allocTic())) {
        release(t.import);
        t.import = nullptr;
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::createPlanarTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                 uint64_t luma, uint32_t lumaPitch, uint64_t chroma, uint32_t chromaPitch,
                                 uint32_t width, uint32_t height, Texture &t)
{
    constexpr uint32_t kRGBA8Unorm = 70;
    if (!width || !height || lumaPitch < width || chromaPitch < 2 * width ||
        luma + uint64_t(lumaPitch) * height > allocBytes || chroma + uint64_t(chromaPitch) * height > allocBytes ||
        (uintptr_t(base) & (pageSize() - 1)))
        return false;
    Import *planes;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        planes = importSurface(surfaceId, base, (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1), surface);
    }
    if (!planes)
        return false;
    if (!createTexture(width, height, kRGBA8Unorm, t)) {  // the image: pool memory, its texture header
        std::lock_guard<raw::Mutex> l(lock_);
        release(planes);
        return false;
    }
    t.import = planes;
    t.planar = true;
    t.addressMap = 2;
    t.planes = {t.import->bo.va + luma, t.import->bo.va + chroma, lumaPitch, chromaPitch};
    return true;
}

Pipeline *Device::standInPipeline(bool store)
{
    return ownPipeline(store ? "raw.two_plane.two_plane_store" : "raw.two_plane.two_plane_load");
}

bool Device::createPlanarTarget(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, Texture &t)
{
    constexpr uint32_t kR8Unorm = 10, kRG8Unorm = 30, kRGBA16Float = 115;
    if (!t.planar || !t.import || !standInPipeline(false) || !standInPipeline(true))
        return false;
    const uint64_t at = t.import->bo.va;
    Texture *luma = new Texture, *chroma = new Texture, *standIn = new Texture;
    const bool made = createSurfaceTexture(surfaceId, base, allocBytes, surface, t.planes.luma - at, t.width, t.height,
                                           t.planes.lumaPitch, kR8Unorm, *luma) &&
                      createSurfaceTexture(surfaceId, base, allocBytes, surface, t.planes.chroma - at, t.width,
                                           t.height, t.planes.chromaPitch, kRG8Unorm, *chroma) &&
                      createTexture(t.width, t.height, kRGBA16Float, *standIn, false, true) && standIn->renderable &&
                      (luma->renderable || luma->shadowTarget) && (chroma->renderable || chroma->shadowTarget);
    if (!made) {
        for (Texture *x : {luma, chroma, standIn}) {
            destroyTexture(*x);
            delete x;
        }
        return false;
    }
    t.luma = luma;
    t.chroma = chroma;
    t.planarStandIn = standIn;
    return true;
}

bool Device::createBufferTexture(const Buffer &b, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t format, Texture &t)
{
    // rows and address a multiple of 16, as a surface's (createSurfaceTexture): the texture header's 32 through `sampled`
    if (!shape(width, height, format, t) || blockCompressed(formatInfo(format)) || (pitch & 15) ||
        pitch < width * t.bpp || offset > b.length || uint64_t(pitch) * height > b.length - offset ||
        offset > UINT64_MAX - b.va || ((b.va + offset) & 15))
        return false;
    // rows or a start 16 bytes off 32, which the texture header cannot address: sampled through a pool image of its
    // texels, as a surface texture's are (createSurfaceTexture)
    Texture *sampled = nullptr;
    if ((pitch | (b.va + offset)) & 31) {
        sampled = new Texture;
        if (!createTexture(width, height, format, *sampled, false, true)) {
            delete sampled;
            return false;
        }
    }
    std::lock_guard<raw::Mutex> l(lock_);
    t.pitch = pitch;
    t.va = b.va + offset;
    t.storage = {t.va, uint64_t(pitch) * height, b.hostVisible, b.span.storage};
    t.shadowTarget = t.renderable && !linearTarget(t);
    t.renderable = t.renderable && linearTarget(t);
    t.cpu = b.cpu + offset;
    if (sampled) {  // sampled through its pool image's header
        t.sampled = sampled;
        t.tic = sampled->tic;
        return true;
    }
    if (!(t.tic = allocTic()))
        return false;
    writeTic(t);
    return true;
}

// NVK's texel buffer views start on 16 bytes (NVK_MIN_TEXEL_BUFFER_ALIGNMENT, its minTexelBufferOffsetAlignment) and hold
// at most 2^27 texels (maxTexelBufferElements); the header itself takes a byte address
bool Device::createTexelBuffer(const Buffer &b, uint64_t offset, uint32_t elements, uint32_t format, Texture &t)
{
    constexpr uint64_t kTexelBufferAlign = 16, kMaxElements = 1u << 27;
    const FormatInfo *f = formatInfo(format);
    if (!f || blockCompressed(f) || xrForm(format) != XrForm::None || !elements || elements > kMaxElements ||
        offset > b.length || uint64_t(elements) * f->bpp > b.length - offset || ((b.va + offset) % kTexelBufferAlign))
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    t = {};
    t.format = format;
    t.bpp = f->bpp;
    t.width = t.texelWidth = elements;
    t.height = t.texelHeight = 1;
    t.pitch = elements * f->bpp;
    t.type = fx::kTexOneDBuffer;
    t.va = b.va + offset;
    t.storage = {t.va, uint64_t(t.pitch), b.hostVisible, b.span.storage};
    t.cpu = b.cpu ? b.cpu + offset : nullptr;
    if (!(t.tic = allocTic()))
        return false;
    writeTic(t);
    return true;
}

// A one-plane 4:2:2 YCbCr texture (YcbcrPlan::onePlane): its image and the luma and chroma pool images its conversion reads,
// which BlitEncoder::splitPlane fills from the surface's plane
bool Device::createOnePlaneYcbcr(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                 const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t)
{
    const uint64_t mapped = (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1);
    const uint64_t pairs = plan.chromaWidth;
    if (!pairs || !height || plan.lumaPitch < 16 || (plan.lumaPitch & 15) || (plan.luma & 15) ||
        plan.luma + uint64_t(plan.lumaPitch) * (height - 1) + 4 * pairs > std::min(plan.readable, mapped) ||
        (uintptr_t(base) & (pageSize() - 1)))
        return false;
    Import *planes;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        planes = importSurface(surfaceId, base, mapped, surface);
    }
    if (!planes)
        return false;
    Texture *y = new Texture, *c = new Texture;
    if (!createTexture(plan.lumaWidth, height, plan.lumaFormat, *y) ||
        !createTexture(plan.chromaWidth, plan.chromaHeight, plan.chromaFormat, *c) || y->blockLinear ||
        c->blockLinear || !createTexture(width, height, plan.image, t, false, true)) {
        for (Texture *x : {y, c}) {
            destroyTexture(*x);
            delete x;
        }
        std::lock_guard<raw::Mutex> l(lock_);
        release(planes);
        return false;
    }
    t.import = planes;
    t.planes = {planes->bo.va + plan.luma, 0, plan.lumaPitch, 0};
    t.onePlane = true;
    t.luma = y;
    t.chroma = c;
    memcpy(t.shape, plan.shape, sizeof t.shape);
    t.ycbcr = plan.output;
    t.addressMap = plan.addressMap;
    ycbcrImageHeader(t, plan.output, plan.shape[2]);
    return true;
}

bool Device::createYcbcrTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t)
{
    if (!ycbcrPipeline(plan.output))
        return false;
    if (plan.onePlane)
        return createOnePlaneYcbcr(surfaceId, base, allocBytes, surface, plan, width, height, t);
    Texture *y = new Texture, *c = plan.chromaWidth ? new Texture : nullptr;
    auto undo = [&] {
        for (Texture *x : {y, c})
            if (x) {
                destroyTexture(*x);
                delete x;
            }
        return false;
    };
    if (!createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.luma, plan.lumaWidth, height, plan.lumaPitch,
                              plan.lumaFormat, *y, false, plan.readable))
        return undo();
    if (c && !createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.chroma, plan.chromaWidth,
                                   plan.chromaHeight, plan.chromaPitch, plan.chromaFormat, *c, false, plan.readable))
        return undo();
    if (!createTexture(width, height, plan.image, t, false, true))  // block-linear, as NVK makes m2v's converted image
        return undo();
    if (plan.nativePlanes) {
        y->nativePlane = 0;
        if (c)
            c->nativePlane = 1;
    }
    t.luma = y;
    t.chroma = c;
    memcpy(t.shape, plan.shape, sizeof t.shape);
    t.ycbcr = plan.output;
    t.addressMap = plan.addressMap;
    ycbcrImageHeader(t, plan.output, plan.shape[2]);
    return true;
}

void Device::ycbcrImageHeader(Texture &t, uint8_t output, uint32_t packedCodes)
{
    std::lock_guard<raw::Mutex> l(lock_);
    uint32_t *w = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic);
    if (ycbcrLumaFirst(output)) {
        constexpr uint8_t kLumaFirst[4] = {3, 4, 2, 5};  // the codes image's green, blue, red: Y, Cb, Cr
        w[0] = swizzledWord0(w[0], kLumaFirst);
    }
    // its alpha read as one, as m2v's view of its own converted image reads it (W_SOURCE ONE_FLOAT; the conversion writes 1
    // there too: the same texels, now the same header)
    w[0] = (w[0] & ~(7u << 28)) | 7u << 28;
    // a packed plane's channels past its codes read 0, as Metal reads R10Unorm_PACKED and RG10Unorm_PACKED (Z_SOURCE, and
    // Y_SOURCE of a one-code texel, ZERO)
    if (output == YcbcrPackedPlane)
        w[0] &= ~(7u << 25 | (packedCodes == 1 ? 7u << 22 : 0u));
}

}  // namespace raw
