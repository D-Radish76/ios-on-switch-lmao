// SPDX-License-Identifier: MIT
#import "RawMetalPassAttachments.h"
#import "RawMetalMultisample.h"
#import "RawMetalRotated.h"
#import "RawMetalTargetViews.h"
#include "raw_attachment_target.h"
#include "raw_format_standin.h"
#include "raw_probe_ops.h"
#include "raw_stencil_bits.h"
#include "raw_zeta_colour.h"

id<MTLTexture> rmZetaAttachment(MTLRenderPassDescriptor *pass)
{
    return pass.depthAttachment.texture ?: pass.stencilAttachment.texture;
}

bool rmResolvesStore(MTLStoreAction a)
{
    return a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve;
}

MTLStoreAction rmKeptStore(MTLStoreAction a)
{
    if (!rmResolvesStore(a))
        return a;
    return a == MTLStoreActionMultisampleResolve ? MTLStoreActionDontCare : MTLStoreActionStore;
}

const raw::Texture *rmZetaPart(RMTexture *t, NSUInteger level, NSUInteger slice, raw::Texture &part)
{
    if ((!level && !slice) || t->_raw.memoryless)
        return &t->_raw;
    if (!level) {
        part = raw::layersFrom(t->_raw, uint32_t(slice));
        return &part;
    }
    RMTexture *owner = t->_levelsOwner ?: t;
    if (!raw::chainLevelImage(owner->_raw, t->_levelsBase + uint32_t(level), part))
        return nullptr;
    part = raw::layersFrom(part, t->_slicesBase + uint32_t(slice));
    part.layers = std::max(t->_raw.layers, 1u) - uint32_t(slice);
    return &part;
}

uint32_t rmZetaPartTic(RMTexture *t, NSUInteger level, NSUInteger slice, const raw::Texture &part)
{
    RMTexture *owner = level ? t->_levelsOwner ?: t : t;
    const uint32_t base = level ? t->_levelsBase : 0, layer = level ? t->_slicesBase : 0;
    return gRM->raw->attachmentTic(owner->_raw, part, base + uint32_t(level), layer + uint32_t(slice));
}

// An aspect's resolve into a level and slice of a one-sample texture holding the aspect, of any zeta format: an M4 Max
// resolves a Depth32Float_Stencil8's depth into a Depth32Float, a Depth32Float's into a Depth32Float_Stencil8 (its stencil
// kept) and a Depth32Float_Stencil8's stencil into a Stencil8, into a level or a slice too (raw/test/passformsoracle.mm
// zeta_resolves). A memoryless one fails the pass first (rmZetaResolvesIntoMemoryless).
static bool zetaResolveTarget(MTLRenderPassAttachmentDescriptor *a, RMTexture *from)
{
    RMTexture *r = (RMTexture *)a.resolveTexture;
    raw::Texture part;
    return from->_raw.samples > 1 && [(id)r isKindOfClass:[RMTexture class]] && r->_rawOk && r->_raw.zeta &&
           r->_raw.samples == 1 && !r->_raw.memoryless && !a.resolveDepthPlane &&
           rmZetaPart(r, a.resolveLevel, a.resolveSlice, part);
}

raw::StencilSample rmStencilPick(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    return raw::stencilResolveSample(uint32_t(pass.stencilAttachment.stencilResolveFilter),
                                     uint32_t(d.depthResolveFilter), d.texture != nil, rmResolvesStore(d.storeAction));
}

// A depth resolve the raw path draws after the pass (the pack's depth resolve state of its format and filter), and a stencil
// resolve (raw_stencil_bits.h: a draw a bit of the pack's stencil bits state)
static bool rawDepthResolve(MTLRenderPassDepthAttachmentDescriptor *d, RMTexture *from)
{
    return zetaResolveTarget(d, from) &&
           gRM->raw->depthResolvePipeline(((RMTexture *)d.resolveTexture)->_raw.format, uint32_t(d.depthResolveFilter));
}

// A separate pair's stencil resolve reads the pass's Depth32Float_Stencil8 (RawMetalRangeForms.mm separateZeta)
static bool rawStencilResolve(MTLRenderPassDescriptor *pass, RMTexture *from, bool separate)
{
    const uint32_t read = separate ? raw::kZetaFormat : from->_raw.format;
    return zetaResolveTarget(pass.stencilAttachment, from) &&
           gRM->raw->stencilBitsPipeline(read, from->_raw.samples, rmStencilPick(pass));
}

bool rmSeparateZeta(MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *s = pass.stencilAttachment;
    return d.texture && s.texture && (d.texture != s.texture || d.level != s.level || d.slice != s.slice);
}

// Why `t`'s level `level`, slice `slice` is no zeta a pass renders whole aspects of, null where it is one
static const char *zetaPartMissing(RMTexture *t, NSUInteger level, NSUInteger slice)
{
    raw::Texture part;
    if (!rmZetaPart(t, level, slice, part))
        return "a depth/stencil level of a texture whose levels the raw path holds no chain of";
    return nullptr;
}

RMZetaForm rmRawDepthStencil(MTLRenderPassDescriptor *pass, bool separate, const char **why)
{
    MTLRenderPassDepthAttachmentDescriptor *d = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *s = pass.stencilAttachment;
    // a colour texture as the depth attachment alone, whole (raw_zeta_colour.h colourAsDepth)
    const bool colourDepth = !separate && d.texture && !s.texture && !d.level && !d.slice &&
                             ((RMTexture *)d.texture)->_rawOk && raw::colourAsDepth(((RMTexture *)d.texture)->_raw);
    for (id<MTLTexture> x : {d.texture, s.texture})
        if (x && !colourDepth && (!((RMTexture *)x)->_rawOk || !((RMTexture *)x)->_raw.zeta))
            return RMZetaForm::NotRaw;
    RMTexture *t = (RMTexture *)rmZetaAttachment(pass);
    const bool depth = separate || colourDepth || raw::zetaHasDepth(t->_raw.format),
               stencil = separate || raw::zetaHasStencil(t->_raw.format);
    *why = nullptr;
    if (separate) {
        const raw::Texture &x = ((RMTexture *)d.texture)->_raw;
        const bool transient = d.loadAction != MTLLoadActionLoad && rmKeptStore(d.storeAction) != MTLStoreActionStore &&
                               !rmResolvesStore(d.storeAction);
        if (x.format == MTLPixelFormatDepth16Unorm && x.samples > 1 && !transient &&
                 !gRM->raw->depthConvertible(x.samples))
            *why = "a multisampled Depth16Unorm loaded or kept beside a stencil texture, of a sample count the pack "
                   "has no conversion states of";
    } else if ((d.texture && !depth) || (s.texture && !stencil)) {
        *why = "a depth/stencil texture as an aspect its format has none of";
    }
    // a separate pair's resolved depth is stored back into its own texture, which the resolve reads; a memoryless depth and
    // a stencil are resolved from the pass's Depth32Float_Stencil8 itself
    RMTexture *depthOwn = separate ? (RMTexture *)d.texture : t, *stencilOwn = separate ? (RMTexture *)s.texture : t;
    if (!*why && d.texture && d.resolveTexture && rmResolvesStore(d.storeAction) && !rawDepthResolve(d, depthOwn))
        *why = "a depth resolve into a multisampled texture, or of a format and filter the pack has no state of";
    if (!*why && s.texture && s.resolveTexture && rmResolvesStore(s.storeAction) &&
        !rawStencilResolve(pass, stencilOwn, separate))
        *why = "a stencil resolve into a multisampled texture, or of a sample count the pack has no state of";
    for (MTLRenderPassAttachmentDescriptor *a :
         {(MTLRenderPassAttachmentDescriptor *)d, (MTLRenderPassAttachmentDescriptor *)s})
        if (!*why && a.texture && !colourDepth)
            *why = zetaPartMissing((RMTexture *)a.texture, a.level, a.slice);
    return *why ? RMZetaForm::Unbuilt : RMZetaForm::Raw;
}

// A probe's command buffer that resolves by the 2D engine (hw_test_app gpu.x1.twod_resolve) needs no resolve state
static bool resolvesByTwoD(RMCommandBuffer *cb, uint32_t format)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return cb->_probeForms.resolveTwoD && raw::probeTwodFormat(format);
#else
    (void)cb, (void)format;
    return false;
#endif
}

// A stand-in attachment's resolve (raw_attachment_target.h formatResolveState): into a stand-in of the resolve texture's by
// its stand-in format's resolve or the format's own state, or for one sample a copy of its stand-in, then stored
static bool standInResolve(const raw::Texture &from, RMTexture *r)
{
    const uint32_t standIn = raw::formatStandIn(from.format);
    if (!rmFormatTarget(r) || r->_raw.samples != 1)
        return false;
    if (from.samples <= 1)
        return true;
    const char *own = raw::formatResolveState(from.format);
    return own ? gRM->raw->ownPipeline(own) != nullptr : gRM->raw->resolvePipeline(standIn, from.samples) != nullptr;
}

bool rmRawResolve(RMCommandBuffer *cb, MTLRenderPassColorAttachmentDescriptor *a, RMTexture *from, RMPass::Color &facts)
{
    RMTexture *r = (RMTexture *)a.resolveTexture;
    if (![(id)r isKindOfClass:[RMTexture class]] || !r->_rawOk || r->_raw.planar || r->_raw.ycbcr ||
        !raw::resolvesInto(from->_raw.format, r->_raw.format))
        return false;
    // a multisampled resolve texture takes the resolve in each of its samples (RawMetalRangeSeal.mm resolvePasses)
    if (r->_raw.samples != 1 && !(from->_raw.samples > 1 && !raw::formatStandIn(from->_raw.format) &&
                                  r->_raw.blockLinear && !r->_raw.rotation))
        return false;
    if (raw::formatStandIn(from->_raw.format)) {
        if (!standInResolve(from->_raw, r))
            return false;
    } else if (!(r->_raw.renderable || r->_raw.shadowTarget)) {
        return false;
    } else if (from->_raw.samples <= 1) {
        // the copy reads the pass's own target
        if (from->_raw.rotation || r->_raw.rotation || from->_raw.planarStandIn || from->_raw.planar)
            return false;
    } else if (!(gRM->raw->resolvePipeline(from->_raw.format, from->_raw.samples) ||
                 resolvesByTwoD(cb, from->_raw.format) ||
                 (raw::codesResolvedByTwod(from->_raw.format) && from->_raw.format == r->_raw.format &&
                  (from->_raw.samples == 2 || from->_raw.samples == 4) && !r->_raw.rotation))) {
        return false;
    }
    const bool volume = r->_raw.type == raw::fx::kTexThreeD;
    facts.resolveLevel = a.resolveLevel;
    facts.resolveSlice = volume ? a.resolveDepthPlane : a.resolveSlice;
    raw::Texture part;
    return !(volume ? a.resolveSlice : a.resolveDepthPlane) && facts.resolveLevel < raw::fx::kMaxLevels &&
           facts.resolveSlice <= UINT32_MAX &&
           (raw::wholeAttachment(r->_raw, uint32_t(facts.resolveLevel), uint32_t(facts.resolveSlice)) ||
            rawAttachmentPart(r, uint32_t(facts.resolveLevel), uint32_t(facts.resolveSlice), part));
}

bool rmFormatTarget(RMTexture *x)
{
    const raw::Texture &t = x->_raw;
    return x->_rawOk && raw::formatStandIn(t.format) && raw::formatStandInSamples(t.format, t.samples, t.memoryless);
}

// A view of several layers or levels is no target as a whole; the part a pass names of it is, where its format is one the
// GM20B renders (raw_attachment_target.h)
static bool partTarget(RMTexture *x, NSUInteger level, NSUInteger slice)
{
    raw::Texture part;
    const raw::FormatInfo *f = raw::formatInfo(x->_raw.format);
    return f && f->rt && !raw::wholeAttachment(x->_raw, uint32_t(level), uint32_t(slice)) &&
           rawAttachmentPart(x, uint32_t(level), uint32_t(slice), part);
}

bool rmWholeViewTarget(RMTexture *x)
{
    const raw::Texture &t = x->_raw;
    const raw::FormatInfo *f = raw::formatInfo(t.format);
    return x->_viewPart && f && f->rt && t.blockLinear && !t.renderable && !t.shadowTarget && !t.zeta &&
           (t.type == raw::fx::kTexTwoD || t.type == raw::fx::kTexOneD) && t.samples <= 1;
}

const char *rmColorTargetMissing(RMTexture *x, NSUInteger level, NSUInteger slice)
{
    const raw::Texture &t = x->_raw;
    raw::Texture buffer;
    if (t.renderable || t.shadowTarget || rmFormatTarget(x) || partTarget(x, level, slice) ||
        raw::texelBufferTarget(t, buffer) || raw::planeInBlocks(t, uint32_t(level), uint32_t(slice)) ||
        rmWholeViewTarget(x))
        return nullptr;
    if (t.type == raw::fx::kTexOneDBuffer)
        return "a texture buffer past 16384 texels as a colour attachment, which an M4 Max fails Internal";
    if (raw::formatStandIn(t.format))
        return "a multisampled target of a format the GM20B renders none of, of a sample count the pack has no "
               "per-sample load and store states of";
    return "a colour attachment the raw path renders no form of";
}

const char *rmColorPartMissing(RMTexture *x, NSUInteger level, NSUInteger slice, bool resolving)
{
    const raw::Texture &t = x->_raw;
    raw::Texture part;
    const bool whole = raw::wholeAttachment(t, uint32_t(level), uint32_t(slice));
    if (resolving && t.samples > 1 && !whole)
        return "a resolved part of a multisampled texture, which this device makes no layers or levels of";
    if (whole || raw::texelBufferTarget(t, part) || raw::planeInBlocks(t, uint32_t(level), uint32_t(slice)) ||
        rawAttachmentPart(x, uint32_t(level), uint32_t(slice), part))
        return nullptr;
    if (!t.blockLinear && raw::formatStandIn(t.format))
        return "a slice of a linear array of a format the GM20B renders none of: its stand-in reads block-linear "
               "headers";
    return "a level of a view of several levels past its first, which the raw path binds through no levels owner";
}

const char *rmPlanarTargetRefusal(RMTexture *x, MTLRenderPassColorAttachmentDescriptor *a)
{
    if (x->_raw.planar && !x->_raw.planarStandIn &&
        !(gRM->raw->standInPipeline(false) && gRM->raw->standInPipeline(true)))
        return "a planar texture as a render target: the pack has no two-plane load or store state";
    return x->_raw.ycbcr ? ycbcrTargetPassRefusal(x->_raw, a) : nullptr;
}
