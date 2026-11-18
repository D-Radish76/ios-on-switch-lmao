// SPDX-License-Identifier: MIT
#import "RawMetalRotated.h"
#import "RawMetalXrPlanes.h"
#include "raw_format_standin.h"
#include "raw_surface_refusal.h"

static NSUInteger descriptorRotation(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
}

static void reportFirst(const RMTexture *t, MTLTextureDescriptor *d)
{
    static std::atomic<bool> said{false};
    if (said.exchange(true))
        return;
    os_log(rawLog(),
           "raw: renders into a turned plane (first: rotation %u, frame %lux%lu over plane %ux%u, %{public}s)",
           unsigned(t->_raw.rotation), (unsigned long)d.width, (unsigned long)d.height, t->_raw.width, t->_raw.height,
           t->_raw.renderable ? (t->_raw.blockLinear ? "block-linear target" : "pitch target")
                              : "through a block-linear shadow");
}

// An M4 Max's Metal answers such a descriptor with its validation's "a non-zero rotation set but is not marked as
// framebufferOnly", and with framebufferOnly set its driver's "Buffer texture has invalid rotation"
bool refuseTurnedOffSurface(MTLTextureDescriptor *d)
{
    const NSUInteger turn = descriptorRotation(d);
    if (!turn)
        return false;
    SWITCH_REFUSE("raw", "tex.turned",
                  "no %lux%lu texture of format %lu and rotation %lu over no surface: nil, as Metal makes none (a "
                  "rotation is a framebuffer-only surface plane's; MTLDevice.h newTextureWithDescriptor:, nullable)",
                  (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)turn);
    return true;
}

// Drawn through stand-ins or store states laid out as their planes, or held in its planes, whatever its format's own
// header
static bool planarFormat(uint32_t format)
{
    return raw::isYcbcrFormat(format) || raw::packedPlaneChannels(format) || raw::twoPlaneXr(format);
}

// RGB10A8_2P_XR10 or its sRGB twin turned: over plane 0 of two whose rows hold the plane's words and alpha bytes, or
// over one plane alone (RawMetalXrPlanes.h)
static bool turnedXrShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, const raw::Rotation &r)
{
    const size_t planes = IOSurfaceGetPlaneCount(s);
    if (!xrPlanesStorable(d))
        return false;
    if (planes <= 1 || plane)
        return true;
    return planes == 2 && IOSurfaceGetBytesPerRowOfPlane(s, 0) >= 4 * r.planeWidth() &&
           IOSurfaceGetBytesPerRowOfPlane(s, 1) >= r.planeWidth() && IOSurfaceGetHeightOfPlane(s, 1) >= r.planeHeight();
}

// Every format Metal renders turns alike, the plane of several of a planar surface as one plane alone (an M4 Max, each
// rotation: m2v-textures4 probes/reads.mm and more.mm landings). A format the GM20B renders none of is drawn turned into its
// stand-in (raw_format_standin.h), a YCbCr one into its stand-in laid out as its planes, which its store states write
// straight; a block-compressed one Metal makes no rotated texture of
bool rotatedShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::Rotation &out)
{
    return turnedFrame(d, s, plane, out) &&
           (!raw::twoPlaneXr(uint32_t(d.pixelFormat)) || turnedXrShape(d, s, plane, out));
}

bool turnedFrame(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::Rotation &out)
{
    const NSUInteger turn = descriptorRotation(d);
    const uint32_t format = uint32_t(d.pixelFormat);
    const raw::FormatInfo *f = raw::formatInfo(format);
    const size_t planes = IOSurfaceGetPlaneCount(s);
    if (!turn || turn > 4 || d.textureType != MTLTextureType2D || d.mipmapLevelCount > 1 || d.sampleCount > 1 ||
        d.arrayLength > 1 || d.depth > 1 ||
        (!planarFormat(format) && (!f || !(f->rt || raw::formatStandIn(format)) || raw::blockCompressed(f))) ||
        (planes > 1 ? plane >= planes : plane != 0))
        return false;
    out.turn = uint8_t(turn);
    out.width = uint32_t(d.width);
    out.height = uint32_t(d.height);
    return (planes ? IOSurfaceGetWidthOfPlane(s, plane) : IOSurfaceGetWidth(s)) >= out.planeWidth() &&
           (planes ? IOSurfaceGetHeightOfPlane(s, plane) : IOSurfaceGetHeight(s)) >= out.planeHeight();
}

MTLTextureDescriptor *planeDescriptor(MTLTextureDescriptor *d, const raw::Rotation &r)
{
    MTLTextureDescriptor *flat = [d copy];
    flat.width = r.planeWidth();
    flat.height = r.planeHeight();
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(flat, sel_registerName("setRotation:"), 0);
    return flat;
}

// A YCbCr texture planned at its plane's extent: its stand-in and store states are laid out as the planes, and a turned
// pass draws into the stand-in turned
static bool createTurnedYcbcr(RMTexture *t, MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane,
                              const raw::Rotation &r)
{
    raw::YcbcrPlan plan;
    if (ycbcrPlan(planeDescriptor(d, r), s, plane, plan))
        return false;
    if (!gRM->raw->createYcbcrTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s, plan,
                                      r.planeWidth(), r.planeHeight(), t->_raw)) {
        raw::surfaceRefusal = "the import of its planes, or no memory or pack state for its image";
        return false;
    }
    if (plan.target)
        makeYcbcrTarget(t->_raw, uint32_t(d.pixelFormat), plan.store);
    return true;
}

static bool createTurnedXr(RMTexture *t, IOSurfaceRef s, NSUInteger plane, const raw::Rotation &r)
{
    return IOSurfaceGetPlaneCount(s) == 2 && !plane ? makeXrPlanes(t, s, r.planeWidth(), r.planeHeight())
                                                    : makeXrWordPlane(t, s, r.planeWidth(), r.planeHeight());
}

bool createRotated(RMTexture *t, MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, const raw::Rotation &r)
{
    const size_t planes = IOSurfaceGetPlaneCount(s);
    void *base = IOSurfaceGetBaseAddress(s);
    const uint64_t offset = planes ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, plane)) - uintptr_t(base) : 0;
    const size_t pitch = planes ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : IOSurfaceGetBytesPerRow(s);
    const uint32_t format = uint32_t(d.pixelFormat);
    // a planar surface's planes stay rows (TegraGM20BLayout.h), so only a surface of one plane asks to be laid out tiled
    const bool made = raw::twoPlaneXr(format) ? createTurnedXr(t, s, plane, r)
                      : planarFormat(format)  ? createTurnedYcbcr(t, d, s, plane, r)
                                              : gRM->raw->createSurfaceTexture(
                                                   IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset,
                                                   r.planeWidth(), r.planeHeight(), uint32_t(pitch), format, t->_raw,
                                                   planes <= 1);
    if (!made)
        return false;
    t->_raw.rotation = r.turn;
    reportFirst(t, d);
    return true;
}

raw::RotatedRefusal rotatedPassRefusal(const RMPass &pass)
{
    raw::PassDesc p;
    const auto rawOf = [](id x) -> raw::Texture * {
        RMTexture *t = (RMTexture *)x;
        return [x isKindOfClass:[RMTexture class]] && t->_rawOk ? &t->_raw : nullptr;
    };
    bool any = false;
    for (unsigned c = 0; c < 8; c++) {
        const RMPass::Color &a = pass.color[c];
        if (!a.texture)
            continue;
        p.color[c].texture = rawOf(a.texture);
        p.color[c].load = a.load == MTLLoadActionLoad    ? raw::ColorAttachment::Load
                          : a.load == MTLLoadActionClear ? raw::ColorAttachment::Clear
                                                         : raw::ColorAttachment::DontCare;
        p.color[c].store = a.store != MTLStoreActionDontCare;
        p.count = c + 1;
        any |= p.color[c].texture && p.color[c].texture->rotation;
    }
    if (!any)
        return raw::RotatedRefusal::None;
    p.depthStencil.texture = rawOf(pass.zeta[0].texture ?: pass.zeta[1].texture);
    p.depthStencil.loaded = pass.zeta[0].load == MTLLoadActionLoad || pass.zeta[1].load == MTLLoadActionLoad;
    p.depthStencil.stored = (pass.zeta[0].texture && pass.zeta[0].store != MTLStoreActionDontCare) ||
                            (pass.zeta[1].texture && pass.zeta[1].store != MTLStoreActionDontCare);
    return raw::turnablePass(p);
}

uint8_t passTurn(const RMPass &pass)
{
    for (const RMPass::Color &a : pass.color) {
        RMTexture *t = (RMTexture *)a.texture;
        if ([a.texture isKindOfClass:[RMTexture class]] && t->_rawOk && t->_raw.rotation)
            return t->_raw.rotation;
    }
    return 0;
}
