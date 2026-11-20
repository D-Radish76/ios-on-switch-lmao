// SPDX-License-Identifier: MIT
// newTextureWithDescriptor:iosurface:plane: on the raw path: a texture over a surface plane's own bytes, an image its
// conversion draws from the planes, or Metal's, the identity's or GM20B's nil with one line
#import "RawMetalInternal.h"
#import "RawMetalPlainRows.h"
#import "RawMetalRotated.h"
#import "RawMetalSurfaceRefusals.h"
#import "RawMetalXrPlanes.h"
#include "raw_surface_note.h"
#include "raw_surface_refusal.h"
#include "raw_texture_shape.h"
#include "raw_ycbcr.h"
#include "raw_zeta.h"
#include <mach/vm_page_size.h>

namespace {

// RGB8_444_2P (542) over plane 0 of a two-plane 4:4:4 surface of one byte a luma texel and two a chroma pair (CoreVideo's
// '_RB4', the widget and poster snapshots Core Animation samples): Metal reads its codes, red Cr, green Y, blue Cb, alpha 1
// (m2v ycbcr.rs), which the raw path holds in an RGBA8 image it writes from the planes with the copy engine before the work that
// samples it (raw::BlitEncoder::convertPlanar). A 2D texture no taller than the surface and as wide as its rows hold (the
// planes' top-left corner, as Metal reads and stores it), one level, no rotation, sampled or rendered (WidgetRenderer's
// snapshots: through its stand-in, raw_planar_target.h), of any usage. Its stand-in is pool memory (8 bytes a texel), held for a
// render target usage alone: any other usage's pass takes one of its command buffer's, so none sits under every snapshot Core
// Animation samples.
bool planar444(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);
    return d.pixelFormat == kRGB8_444_2P && plane == 0 && IOSurfaceGetPlaneCount(s) == 2 &&
           d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 &&
           !descriptorRotation(d) && IOSurfaceGetBytesPerRowOfPlane(s, 0) >= d.width &&
           IOSurfaceGetHeightOfPlane(s, 0) >= d.height &&
           IOSurfaceGetBytesPerRowOfPlane(s, 1) >= 2 * d.width && IOSurfaceGetHeightOfPlane(s, 1) >= d.height &&
           IOSurfaceGetBytesPerElementOfPlane(s, 0) == 1 && IOSurfaceGetBytesPerElementOfPlane(s, 1) == 2;
}

// Whether the GPU may write a texture of this descriptor (Unknown: any use)
bool writesTexture(MTLTextureDescriptor *d)
{
    return d.usage == MTLTextureUsageUnknown || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
}

// A plane's rows as Metal's GPU reads them: its bytesPerRow rounded down to 16 (a single row's whatever covers it)
size_t gpuRows(size_t pitch, MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    const size_t packed = d.width * (f ? f->bpp : 0);
    return d.height == 1 ? std::max(pitch & ~size_t(15), (packed + 15) & ~size_t(15)) : pitch & ~size_t(15);
}

// A plane whose offset or rows are off 16, made at Metal's GPU layout (both rounded down to 16): its CPU's rows are the
// plane's own
void keepCpuRows(RMTexture *t, uint8_t *rows, size_t pitch)
{
    if (t->_rawOk && !t->_raw.blockLinear && ((uintptr_t(rows) | pitch) & 15)) {
        t->_cpuRows = rows;
        t->_cpuPitch = pitch;
    }
}

// A block-compressed 2D texture over a surface's rows of blocks: one level and sample, sampled alone (Metal neither renders
// nor stores a compressed format)
bool blockSurface(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    return raw::blockCompressed(raw::formatInfo(uint32_t(d.pixelFormat))) && d.textureType == MTLTextureType2D &&
           d.mipmapLevelCount == 1 && d.sampleCount == 1 && !plane && IOSurfaceGetPlaneCount(s) <= 1 &&
           IOSurfaceGetWidth(s) && !(d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
}

// Whether a texture of this descriptor is made to be rendered into or stored to, as the kernel's layout decision takes it
// (TegraGM20BLayout.h): a surface whose first texture says Unknown is left pitch, as the CPU may produce it
bool producesTexture(MTLTextureDescriptor *d)
{
    return d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite);
}

// RGB10A8_2P_XR10 or its sRGB twin over a surface of one plane, or over the last plane of several: BGR10_XR's words
// (RawMetalXrPlanes.h xrWordsFormat). An M4 Max's passes and kernel stores write them with bits 30 and 31 clear (m2v-textures4
// probes/more.mm ff), which BGR10_XR's write sets, so the raw path takes one sampled alone
bool xrWordsShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    return raw::twoPlaneXr(uint32_t(d.pixelFormat)) && xrLastPlane(s, plane) &&
           d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 &&
           !descriptorRotation(d) && !writesTexture(d);
}

// Such a texture: the plane under BGR10_XR's (BGR10_XR_sRGB's) header, its stand-in's header reading alpha 0
bool makeXrWords(RMTexture *t, IOSurfaceRef s)
{
    MTLTextureDescriptor *d = t->_descriptor;
    uint64_t offset;
    size_t pitch;
    planeAt(s, t->_plane, offset, pitch);
    // at Metal's GPU layout, offset and rows rounded down to 16, its CPU copies at the plane's (probes/xr.txt 5k-5n); rows
    // that overlap are read at that pitch through a pool the copy engine gathers, to the surface's last page
    const size_t page = vm_page_size, mapped = (IOSurfaceGetAllocSize(s) + page - 1) & ~(page - 1);
    const bool overlaps = (pitch & ~15ul) < 4 * d.width;
    if (!gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s,
                                        offset & ~15ull, uint32_t(d.width), uint32_t(d.height), uint32_t(pitch & ~15ul),
                                        xrWordsFormat(uint32_t(d.pixelFormat)), t->_raw, false,
                                        overlaps && (pitch & ~15ul) ? mapped : 0))
        return false;
    if (!t->_raw.blockLinear && ((offset | pitch) & 15)) {
        t->_cpuRows = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s)) + offset;
        t->_cpuPitch = pitch;
    }
    if (!keepExtendedRange(d, t->_raw, false) || !t->_raw.standIn) {
        raw::surfaceRefusal = "no memory or texture header for the stand-in it is sampled through";
        gRM->raw->destroyTexture(t->_raw);
        return false;
    }
    readAlphaZero(*t->_raw.standIn);
    return true;
}

// A texture buffer over the plane's bytes in order, an M4 Max's read of one (its rows play no part). RGB10A8_2P_XR10's and
// its sRGB twin's are BGR10_XR's words, which an M4 Max reads with alpha 0 and whose kernels and fragment functions store
// them with bits 30 and 31 clear and nothing else: a program that stores one is compiled for its format, whose texel
// buffer the translator holds as the words alone, and samples read its stand-in, alpha 0
bool makeSurfaceTexelBuffer(RMTexture *t, IOSurfaceRef s, uint64_t offset, size_t pitch)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const uint32_t format = uint32_t(d.pixelFormat);
    const bool words = raw::twoPlaneXr(format);
    if (d.width > UINT32_MAX || pitch > UINT32_MAX ||
        !gRM->raw->createSurfaceTexelBuffer(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s,
                                            offset, uint32_t(pitch), uint32_t(d.width),
                                            words ? xrWordsFormat(format) : format, t->_raw))
        return false;
    if (words && t->_raw.standIn)
        readAlphaZero(*t->_raw.standIn);
    return true;
}

#if SWITCH_FRAME_TIMELINE
void noteCompositorSurface(MTLTextureDescriptor *d, IOSurfaceRef s, const void *frame)
{
    constexpr size_t kHalfScreen = 1280 * 720 / 2;
    if (!producesTexture(d) || IOSurfaceGetWidth(s) * IOSurfaceGetHeight(s) < kHalfScreen || !gRM || !gRM->raw ||
        gRM->raw->role() != raw::Device::Role::RenderServer || !raw::noteAllowed())
        return;
    raw::postSurfaceNote(s, frame);
}
#endif

}  // namespace

// A YCbCr texture's plan (raw_ycbcr.h): Metal reads it converted by the surface's range and matrix, that through the sRGB curve,
// or its codes, which the raw path draws into an image of its own with the pack's conversion state (pack/ycbcr.metal, exact
// against Metal's reads: test/ycbcroracle.mm) before the work that samples it. Null when taken, else the rule that refused it.
const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::YcbcrPlan &out)
{
    raw::YcbcrFacts f;
    f.pixelFormat = uint32_t(d.pixelFormat);
    f.fourcc = IOSurfaceGetPixelFormat(s);
    const size_t count = IOSurfaceGetPlaneCount(s);
    if (plane >= std::max<size_t>(count, 1))
        return "a plane the surface does not have";
    // over a surface of more than two planes, the texture's plane and the one after it as planes 0 and 1 (raw_ycbcr.h)
    const size_t first = count > 2 ? plane : 0;
    f.plane = uint32_t(plane - first);
    f.planes = uint32_t(count > 2 ? std::min<size_t>(count - plane, 2) : count);
    f.width = uint32_t(d.width);
    f.height = uint32_t(d.height);
    f.surfaceWidth = uint32_t(IOSurfaceGetWidth(s));
    f.surfaceHeight = uint32_t(IOSurfaceGetHeight(s));
    const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
    for (size_t k = 0; k < 2 && k < f.planes; k++) {
        f.offset[k] = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, first + k)) - base;
        f.pitch[k] = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, first + k));
    }
    if (!f.planes)  // a one-plane format's surface of no planes ('2vuy', 'yuvs')
        f.pitch[0] = uint32_t(IOSurfaceGetBytesPerRow(s));
    f.allocBytes = IOSurfaceGetAllocSize(s);
    f.mappedBytes = (f.allocBytes + vm_page_size - 1) & ~uint64_t(vm_page_size - 1);
    f.sparseTail = gRM->raw->kernel().sparse();
    f.buffer = d.textureType == MTLTextureTypeTextureBuffer;
    if (f.buffer)
        raw::ycbcrBufferExtent(uint32_t(d.width), f.width, f.height);
    f.simple = (d.textureType == MTLTextureType2D || f.buffer) && d.sampleCount == 1 && !descriptorRotation(d);
    f.levels = uint32_t(d.mipmapLevelCount);
    // An M4 Max renders into a texture of any usage as into a render target (raw/test/ycbcrnorenderoracle.mm); the stand-in is
    // memoryless, so every texture that can be one is. A texture buffer is read alone
    f.target = !f.buffer;
    char matrix[32] = "";
    if (CFTypeRef value = IOSurfaceCopyValue(s, CFSTR("IOSurfaceYCbCrMatrix"))) {
        if (CFGetTypeID(value) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)value, matrix, sizeof matrix, kCFStringEncodingUTF8);
        CFRelease(value);
        f.matrix = matrix;
    }
    const char *why = raw::planYcbcr(f, out);
    if (why) {  // sampled, and refused only once a pass renders into it (enc.planarrt)
        f.target = false;
        why = raw::planYcbcr(f, out);
    }
    // the kernel's native view knows the surface's own plane numbers
    if (first)
        out.nativePlanes = false;
    return why;
}

@interface RMDevice (SurfaceTextures)
@end

@implementation RMDevice (SurfaceTextures)

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
    return swizzled([self rmSurfaceTexture:d iosurface:s plane:plane], d.swizzle);
}

- (id<MTLTexture>)rmSurfaceTexture:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
    SWITCH_API(ApiObject);
    if (!s)
        return nil;
    if (raw::ycbcrMetalRefuses(uint32_t(d.pixelFormat), uint32_t(d.width), uint32_t(d.height), d.usage)) {
        SWITCH_REFUSE_DETAIL(
            "raw", "ycbcr.odd", d.pixelFormat,
            "no %lux%lu texture of format %lu and usage 0x%lx: nil, as Metal answers for a subsampled "
            "YCbCr texture of an odd size it may write (MTLDevice.h newTextureWithDescriptor:iosurface:"
            "plane:, nullable)",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage);
        return nil;
    }
#if SWITCH_FRAME_TIMELINE
    gSurfaceTextures.fetch_add(1, std::memory_order_relaxed);
    noteCompositorSurface(d, s, __builtin_frame_address(0));
#endif
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = RMTextureSurface;
    t->_surface = (IOSurfaceRef)CFRetain(s);
    t->_surfaceWrites = holdSurfaceWrites(s);
    t->_plane = plane;
    raw::surfaceRefusal = nullptr;
    raw::YcbcrPlan ycbcr;
    raw::Rotation turned;
    uint64_t offset;
    size_t pitch;
    planeAt(s, plane, offset, pitch);
    void *base = IOSurfaceGetBaseAddress(s);  // valid while the surface lives (the import retains it)
    if (rotatedShape(d, s, plane, turned)) {
        t->_rawOk = createRotated(t, d, s, plane, turned);
    } else if (planar444(d, s, plane)) {  // RGB8_444_2P over a 4:4:4 two-plane surface: an RGBA8 image of its codes
        const uintptr_t at = uintptr_t(base);
        t->_rawOk = gRM->raw->createPlanarTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)), uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1)), uint32_t(d.width), uint32_t(d.height), t->_raw);
        if (!t->_rawOk)
            raw::surfaceRefusal = raw::surfaceRefusal ?: "the import of its planes, or no memory for its image";
        if (t->_rawOk && (d.usage & MTLTextureUsageRenderTarget) &&
            !gRM->raw->createPlanarTarget(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, t->_raw))
            SWITCH_REFUSE_DETAIL("raw", "planar.target", 0,
                                 "no stand-in held for a %lux%lu RGB8_444_2P render target: its passes take one of "
                                 "their command buffer's",
                                 (unsigned long)d.width, (unsigned long)d.height);
    } else if (xrPlanesShape(d, s, plane)) {
        t->_rawOk = makeXrPlanes(t, s, uint32_t(d.width), uint32_t(d.height));
    } else if (xrWordPlaneShape(d, s, plane)) {
        t->_rawOk = makeXrWordPlane(t, s, uint32_t(d.width), uint32_t(d.height));
    } else if (xrWordsShape(d, s, plane)) {
        t->_rawOk = makeXrWords(t, s);
    } else if ((raw::isYcbcrFormat(uint32_t(d.pixelFormat)) || raw::packedPlaneChannels(uint32_t(d.pixelFormat))) &&
               !ycbcrPlan(d, s, plane, ycbcr)) {
        // a YCbCr texture or a packed 10-bit plane: an image the pack's conversion state draws
        t->_rawOk = gRM->raw->createYcbcrTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, ycbcr,
                                                 uint32_t(d.width), uint32_t(d.height), t->_raw);
        if (t->_rawOk && ycbcr.target)
            makeYcbcrTarget(t->_raw, uint32_t(d.pixelFormat), ycbcr.store);
    } else if (raw::SurfaceImage image; (!rawShape(d) || !IOSurfaceGetWidth(s)) && laidOutSurface(d, s, plane, image)) {
        // a shared texture in the device's layout of its descriptor (RawMetalSharedTextures.mm): a surface of no rows
        t->_rawOk = gRM->raw->createSurfaceImage(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, image,
                                                 uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat),
                                                 t->_raw);
    } else if (d.textureType == MTLTextureTypeTextureBuffer) {
        t->_rawOk = !texelBufferRowsRefusal(d, s) && makeSurfaceTexelBuffer(t, s, offset, pitch);
    } else if (blockSurface(d, s, plane)) {
        // its blocks in place under a pitch header (raw_surface_textures.cpp createSurfaceTexture), rows off 32 bytes
        // through a block-linear pool image the copy engine fills from them, refreshed as the surface's seed moves
        t->_rawOk = gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset,
                                                   uint32_t(d.width), uint32_t(d.height), uint32_t(pitch),
                                                   uint32_t(d.pixelFormat), t->_raw, false);
    } else if (plainRowsShape(d, s, plane)) {
        t->_rawOk = makePlainRows(t, s, plane, uint32_t(d.width), uint32_t(d.height));
    } else if (rawShape(d) && !raw::twoPlaneXr(uint32_t(d.pixelFormat)) && !plane &&
               IOSurfaceGetPlaneCount(s) <= 1) {
        // a surface without planes, or one described by a single plane (CoreVideo's chunky surfaces): its plane's bytes
        const size_t gpuPitch = IOSurfaceGetPlaneCount(s) == 1 ? gpuRows(pitch, d) : pitch;
        t->_rawOk = gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset & ~15ull,
                                                   uint32_t(d.width), uint32_t(d.height), uint32_t(gpuPitch),
                                                   uint32_t(d.pixelFormat), t->_raw, producesTexture(d));
        keepCpuRows(t, static_cast<uint8_t *>(base) + offset, pitch);
        if (t->_rawOk && !keepExtendedRange(d, t->_raw, false)) {
            raw::surfaceRefusal = "no memory or texture header for the stand-in it is sampled through";
            gRM->raw->destroyTexture(t->_raw);
            t->_rawOk = false;
        }
    } else if (rawShape(d) && !raw::twoPlaneXr(uint32_t(d.pixelFormat)) && plane < IOSurfaceGetPlaneCount(s)) {
        // a plane as a texture of its own (a video player's R8 luma and RG8 chroma): the plane's bytes, pitch, as CoreVideo
        // lays its planes out for every other reader (never a block-linear layout the kernel would decide for the surface)
        t->_rawOk = gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset & ~15ull,
                                                   uint32_t(d.width), uint32_t(d.height), uint32_t(gpuRows(pitch, d)),
                                                   uint32_t(d.pixelFormat), t->_raw, false);
        keepCpuRows(t, static_cast<uint8_t *>(base) + offset, pitch);
        const OSType fourcc = IOSurfaceGetPixelFormat(s);
        if (t->_rawOk && (fourcc == 0x34323076 || fourcc == 0x34323066))
            t->_raw.nativePlane = uint32_t(plane);
        if (t->_rawOk && !keepExtendedRange(d, t->_raw, false)) {
            raw::surfaceRefusal = "no memory or texture header for the stand-in it is sampled through";
            gRM->raw->destroyTexture(t->_raw);
            t->_rawOk = false;
        }
    }
    if (!t->_rawOk)
        return unmadeSurfaceTexture(d, s, plane);
    if (t->_raw.converted())
        surfaceConverted(t->_surfaceWrites, true);
    if (writesTexture(d))
        declareGpuWrites(t);
#if SWITCH_FRAME_TIMELINE
    if (t->_raw.planar || t->_raw.ycbcr)
        traceSurfaceImage(t, "imported", IOSurfaceGetSeed(s));
#endif
    return (id<MTLTexture>)t;
}

@end
