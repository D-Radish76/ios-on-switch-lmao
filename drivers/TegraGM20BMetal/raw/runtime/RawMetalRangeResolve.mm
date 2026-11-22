// SPDX-License-Identifier: MIT
// The range encode's resolves after a pass: colour (the 2D engine, the pack's resolve states, stand-in formats, into a
// multisampled texture), depth and stencil
#import "RawMetalRangeEncode.h"
#import "RawMetalPassAttachments.h"
#import "RawMetalPassErrors.h"
#import "RawMetalResolveAsk.h"
#import "RawMetalTargetViews.h"
#include "raw_format_standin.h"
#include "raw_samples.h"

static const raw::DepthStencilState &depthWrites()
{
    static const raw::DepthStencilState state = [] {
        raw::DepthStencilDesc desc;
        desc.depthCompare = 7;  // MTLCompareFunctionAlways
        desc.depthWrite = true;
        return raw::DepthStencilState(desc);
    }();
    return state;
}

enum class ResolveEnd : uint8_t {
    Done,
    Internal,
    NoMemory,
    DrawRefused,
};

// A pass of the pack's resolve state `state` sampling `from` into `into` over the render area (an 8-bit sRGB one as its
// UNORM twin, whose codes the state writes); the resolve pass's prologue barrier orders its reads after the pass's writes
static ResolveEnd resolveByState([[maybe_unused]] raw::Device &d, raw::CommandBuffer &rc, raw::Pipeline *state,
                                 const raw::Texture &from, const raw::Texture &into, uint32_t width, uint32_t height,
                                 std::deque<raw::Texture> &targets, const char **why)
{
    targets.push_back(into);
    targets.back().format = raw::resolveTargetFormat(into.format);
    raw::PassDesc p;
    p.color[0].texture = &targets.back();
    p.color[0].load = raw::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    p.width = width;
    p.height = height;
    raw::RenderEncoder *e = rc.render(p, why);
    if (!e)
        return ResolveEnd::NoMemory;
    const uint32_t samples = from.samples;
    e->setPipeline(state);
    e->setTexture(0, &from);
    e->setBytes(2, 0, &samples, sizeof samples);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return ResolveEnd::DrawRefused;
    e->end();
    RAW_DIAGNOSTIC(d.stats.resolveRoute[raw::ResolveByPass][raw::resolveSampleSlot(samples)]++);
    return ResolveEnd::Done;
}

// The 2D engine's resolve of codes (raw_attachment_target.h codesResolvedByTwod) over the render area of any target the
// recording took: straight into a block-linear image, else into a block-linear scratch of `slot`'s that the copy engine
// moves into the pitch target the 2D engine is not given (Texture::shadowTarget)
static bool codesResolve(raw::CommandBuffer &rc, const raw::Texture &from, const raw::Texture &into, uint32_t width,
                         uint32_t height, uint32_t slot, bool keep)
{
    const uint32_t w = std::min({width, from.width, into.width}), h = std::min({height, from.height, into.height});
    if (!into.shadowTarget)
        return rc.resolveTwod(from, into, false, keep, w, h);
    raw::Texture *scratch = rc.turnScratch(into.format, from.width, from.height, slot);
    if (!scratch || !rc.resolveTwod(from, *scratch, false, keep, w, h))
        return false;
    raw::BlitEncoder *b = rc.blit();
    b->copyTexels(*scratch, 0, 0, 0, into, 0, 0, 0, w, h);
    b->end();
    return true;
}

// A resolve of an attachment of a format the GM20B renders none of (raw_attachment_target.h formatResolveState) into
// `into`, a texture of its format (its image or part): the attachment's stand-in resolved in the stand-in's format (the
// 2D engine's or the pack's state, as rule resolve.twod takes for that format) or by the format's own state, a
// one-sample one copied, into a stand-in of `into` of slot kFormatResolveSlot + `attachment` (*made), which the pass's
// stores then store into it. The stand-in is filled from `into` first where the render area leaves some of it, as Metal's
// resolve writes the area alone
static ResolveEnd standInResolve(raw::Device &d, raw::CommandBuffer &rc, RMTexture *owner, const raw::ColorAttachment &into,
                                 const raw::Texture &from, uint32_t attachment, uint32_t width, uint32_t height,
                                 std::deque<raw::Texture> &targets, raw::Texture *&made, const char **why)
{
    const raw::Texture &t = *into.texture;
    const bool covered = width >= t.width && height >= t.height;
    raw::Texture *s = made = raw::formatStandInFor(d, rc, into, 1, raw::kFormatResolveSlot + attachment, !covered, why);
    if (!s)
        return ResolveEnd::Internal;
    if (from.samples <= 1) {
        raw::BlitEncoder *b = rc.blit();
        b->copyTexels(from, 0, 0, 0, *s, 0, 0, 0, std::min(width, s->width), std::min(height, s->height));
        b->end();
        return ResolveEnd::Done;
    }
    const char *own = raw::formatResolveState(t.format);
    if (!own && raw::resolveRoute(resolveAsk(owner, *s, from, width, height)) != raw::ResolveByPass &&
        rc.resolveTwod(from, *s, false, false))
        return ResolveEnd::Done;
    raw::Pipeline *state = own ? d.ownPipeline(own) : d.resolvePipeline(s->format, from.samples);
    if (!state) {
        *why = "a stand-in attachment's resolve the recording took with no resolve state";
        return ResolveEnd::Internal;
    }
    return resolveByState(d, rc, state, from, *s, width, height, targets, why);
}

// Multisampled attachments' resolves (MTLStoreActionMultisampleResolve): after their pass, by the 2D engine (rule
// resolve.twod, and every resolve of codes) or a pass of the pack's resolve state of the format each, into the resolve
// texture over the pass's render area. A resolve into a multisampled texture (Metal's validation error) is made into a
// one-sample image of its format, then written into every sample of it by the 2D engine (CommandBuffer::spreadSamples),
// for a pass that records no draw alone: an M4 Max writes a clear-only pass's resolve so and leaves the texture as it was
// after a drawn pass (raw/test/passoracle.mm resolve_other_count; HW Test gpu.raw.resolve_other_count)
bool RMRangeEncode::resolvePasses()
{
    raw::Device &d = *d_;
    raw::CommandBuffer *rc = rc_;
    const raw::Texture *from[8] = {};
    for (const Resolve &r : resolves_)
        from[r.attachment] = re_->target(r.attachment);
    const raw::Texture *depthFrom =
        depthResolve_.into ? (separateResolved_[0] ? separateResolved_[0] : re_->depthTarget()) : nullptr;
    const raw::Texture *stencilFrom =
        stencilResolve_.into ? (separateResolved_[1] ? separateResolved_[1] : re_->depthTarget()) : nullptr;
    // a memoryless zeta's compressed backing is made without a texture header (Device::compBacking)
    for (const raw::Texture *zeta : {depthFrom, stencilFrom})
        if (zeta && !d.sampleable(*const_cast<raw::Texture *>(zeta))) {
            constexpr const char *kNoHeader = "no texture header for a multisampled zeta's resolve";
            rmPassWorkFails(cb_, ahead_, kNoHeader, MTLCommandBufferErrorOutOfMemory);
            return fail(PredPassDepthStencil, kNoHeader, zeta->format);
        }
    // as far as Metal's Clear reaches (raw_pass_end.cpp): the render area in whole 32-pixel tiles in a pass that drew, the
    // whole attachment in one that did not (an M4 Max: HW Test gpu.raw.resolve_codes_parts' area case; a clear-only
    // pass's clear covers all of a texture past renderTargetWidth and renderTargetHeight); clipped to each resolve's images
    const bool drew = rc->draws != passDraws_;
    const auto reach = [drew](uint32_t area) {
        return drew ? (area + raw::kReachTile - 1) / raw::kReachTile * raw::kReachTile : UINT32_MAX;
    };
    const uint32_t reachWidth = reach(re_->areaWidth()), reachHeight = reach(re_->areaHeight());
    const auto ended = [&](ResolveEnd end, uint32_t format, uint32_t attachment) {
        switch (end) {
        case ResolveEnd::Done:
            return true;
        case ResolveEnd::Internal:
            rmPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorInternal);
            return fail(PredEncPassRender, *why_, format, attachment);
        case ResolveEnd::NoMemory:
            rmPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorOutOfMemory);
            return passRefused(attachment);
        case ResolveEnd::DrawRefused:
            rmOwnDrawFails(cb_, ahead_, *why_, rc->refusal);
            return refused();
        }
        return false;
    };
    for (const Resolve &r : resolves_) {
        raw::Texture part;
        const raw::Texture *into = rawAttachmentTarget(r.into, r.level, r.slice, false, part);
        if (!into) {
            constexpr const char *kNoPart = "a resolve level, slice or plane the recording took and cannot be rendered";
            rmPassWorkFails(cb_, ahead_, kNoPart, MTLCommandBufferErrorInternal);
            return fail(PredEncPassRender, kNoPart, r.attachment);
        }
        const raw::Texture &source = *from[r.attachment];
        const uint32_t width = std::min({reachWidth, source.width, into->width}),
                       height = std::min({reachHeight, source.height, into->height});
        useTexture(rc, r.into, true);
        if (raw::formatStandIn(into->format)) {
            raw::ColorAttachment a;
            const bool whole = into == &r.into->_raw;
            if (!whole) {  // a header of its own, which a fill of its stand-in reads
                resolveTargets_.push_back(part);
                resolveTargets_.back().tic = d.attachmentTic(rawLevelsOwner(r.into)->_raw, part, r.level, r.slice);
            }
            a.texture = whole ? &r.into->_raw : &resolveTargets_.back();
            a.image = whole ? nullptr : &r.into->_raw;
            a.level = r.level;
            a.slice = r.slice;
            raw::Texture *standIn = nullptr;
            const ResolveEnd end = standInResolve(d, *rc, r.into, a, source, r.attachment, width, height,
                                                  resolveTargets_, standIn, why_);
            if (end != ResolveEnd::Done)
                return ended(end, into->format, r.attachment);
            formatStores_.push_back({a, 1, standIn});
            continue;
        }
        if (into->samples > 1 && drew)
            continue;
        if (source.samples <= 1) {
            raw::BlitEncoder *b = rc->blit();
            if (raw::copiesInto(source.format, into->format))
                b->copyTexels(source, 0, 0, 0, *into, 0, 0, 0, std::min(width, into->width),
                              std::min(height, into->height));
            else
                b->copyRedBlueSwapped(source, *into, std::min(width, into->width), std::min(height, into->height));
            b->end();
            continue;
        }
        const raw::Texture *target = into;
        if (into->samples > 1 &&
            !(target = rc->turnScratch(into->format, into->width, into->height,
                                       raw::kFormatStandInTag + raw::kFormatResolveSlot + r.attachment))) {
            *why_ = "no memory for a resolve into a multisampled texture";
            return ended(ResolveEnd::NoMemory, into->format, r.attachment);
        }
        const raw::ResolveAsk ask = resolveAsk(r.into, *target, source, width, height);
        raw::ResolveRoute route = raw::resolveRoute(ask);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (d.probeResolveDraw.load(std::memory_order_relaxed))
            route = raw::ResolveByPass;
        else if (raw::twodResolveComplementShape(ask) && d.probeResolveExact.load(std::memory_order_relaxed))
            route = raw::ResolveByComplement;
#endif
        // a turned target's resolve is its state's turned variant, which reads the frame's position; one across formats
        // (raw::resolvesInto) keeps the attachment's encoding, which the 2D engine's filter of the target's does not
        const bool turned = r.into->_raw.rotation;
        bool done = false;
        if (raw::codesResolvedByTwod(target->format)) {
            if (!codesResolve(*rc, source, *target, width, height,
                              raw::kFormatStandInTag + raw::kFormatResolveSlot + r.attachment, r.keepSamples)) {
                *why_ = "a resolve of codes the 2D engine takes no grid or memory for";
                return ended(ResolveEnd::Internal, target->format, r.attachment);
            }
            done = true;
        } else if (!turned && source.format == target->format && route != raw::ResolveByPass) {
            done = rc->resolveTwod(source, *target, route == raw::ResolveByComplement, r.keepSamples);
        }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (!done && !turned && rc->probeForms().resolveTwoD)
            done = rc->probeResolve(source, *target);
#endif
        if (!done) {
            raw::Pipeline *state = d.resolvePipeline(source.format, source.samples);
            if (!state)
                *why_ = "a resolve the recording took of a format with no resolve state";
            const ResolveEnd end = state ? resolveByState(d, *rc, state, source, *target, width, height,
                                                          resolveTargets_, why_)
                                         : ResolveEnd::Internal;
            if (end != ResolveEnd::Done)
                return ended(end, target->format, r.attachment);
        }
        if (target != into && !rc->spreadSamples(*target, *into, width, height)) {
            *why_ = "a resolve into a multisampled texture of a format the 2D engine takes none of";
            return ended(ResolveEnd::Internal, into->format, r.attachment);
        }
    }
    resolves_.clear();
    if (__unsafe_unretained RMTexture *into = depthResolve_.into) {
        raw::PassDesc p;
        resolveTargets_.emplace_back();
        p.depthStencil.texture =
            const_cast<raw::Texture *>(rmZetaPart(into, depthResolve_.level, depthResolve_.slice, resolveTargets_.back()));
        p.width = depthFrom ? std::min(reachWidth, depthFrom->width) : 0;
        p.height = depthFrom ? std::min(reachHeight, depthFrom->height) : 0;
        useTexture(rc, into, true);
        raw::RenderEncoder *e = depthFrom && p.depthStencil.texture ? rc->render(p) : nullptr;
        if (!e) {
            constexpr const char *kNoPass = "no memory for a depth resolve's pass";
            rmPassWorkFails(cb_, ahead_, kNoPass, MTLCommandBufferErrorOutOfMemory);
            return fail(PredPassDepthStencil, kNoPass, into->_raw.format);
        }
        const uint32_t samples = depthFrom->samples;
        e->setPipeline(d.depthResolvePipeline(into->_raw.format, depthResolve_.filter));
        e->setDepthStencil(&depthWrites());
        e->setTexture(0, depthFrom);
        e->setBytes(2, 0, &samples, sizeof samples);
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_)) {
            rmOwnDrawFails(cb_, ahead_, *why_, rc->refusal);
            return refused();
        }
        e->end();
        depthResolve_ = {};
    }
    if (__unsafe_unretained RMTexture *into = stencilResolve_.into) {
        useTexture(rc, into, true);
        resolveTargets_.emplace_back();
        raw::Texture *target = const_cast<raw::Texture *>(
            rmZetaPart(into, stencilResolve_.level, stencilResolve_.slice, resolveTargets_.back()));
        if (!stencilFrom || !target ||
            !rc->stencilResolve(*target, *stencilFrom, stencilResolve_.pick, std::min(reachWidth, stencilFrom->width),
                                std::min(reachHeight, stencilFrom->height), why_)) {
            const char *why = stencilFrom ? *why_ : "a stencil resolve the recording took without its source";
            rmPassWorkFails(cb_, ahead_, why, MTLCommandBufferErrorInternal);
            return fail(PredPassDepthStencil, why, into->_raw.format);
        }
        stencilResolve_ = {};
    }
    return true;
}
