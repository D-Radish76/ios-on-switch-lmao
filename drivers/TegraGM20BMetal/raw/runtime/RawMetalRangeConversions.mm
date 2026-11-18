// SPDX-License-Identifier: MIT
// The range encode's planar and YCbCr images: the converted textures a range samples are read through images made from
// their planes, before the range where stale and again after each encoder of the range that writes their surface, ahead
// of the next work that samples them. An Apple GPU samples the planes as the encoders before it in the same command buffer
// left them (an M4 Max, 4:2:2, 4:2:0 and code formats, partly redrawn loads and kernels too:
// c38-audit/m2v-planar-plan/probe).
#import "RawMetalRangeEncode.h"
#import "RawMetalPassErrors.h"

namespace {

// The converted texture `use` samples the image of, nil for none: a view samples its texture's image
RMTexture *sampledImage(const RMRangeResource &use)
{
    if (!(use.access & raw::ResourceUse::Sample) || !(use.facts.roles & RMResourceFacts::Converted))
        return nil;
    return rmStorageOwner((__bridge RMTexture *)use.object);
}

// Whether `use` writes the planes `t`'s image is made from: through `t`, a view of it, or any other texture over its
// surface (a plane's own texture, the _sRGB twin)
bool writesPlanesOf(const RMRangeResource &use, RMTexture *t)
{
    if (!(use.access & raw::ResourceUse::Write))
        return false;
    return t->_surfaceWrites ? use.facts.surfaceWrites == t->_surfaceWrites
                             : rmStorageOwner((__bridge RMTexture *)use.object) == t;
}

template <class Visit> void forEachEncoder(const RMCommandPlan &plan, size_t from, size_t to, Visit visit)
{
    for (size_t at = from; at < to; ++at)
        if (RMCommandPlan::isEncoderBegin(plan.operation(at).op)) {
            const size_t end = plan.encoderEnd(at);
            if (!visit(at, end))
                return;
            at = end - 1;
        }
}

bool sampledIn(const RMCommandPlan &plan, size_t from, size_t to, RMTexture *t)
{
    bool sampled = false;
    forEachEncoder(plan, from, to, [&](size_t begin, size_t end) {
        plan.forEachResource(begin, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
            sampled |= sampledImage(use) == t;
        });
        return !sampled;
    });
    return sampled;
}

// Whether the first pass, blit or kernel of the range that reaches `t` writes it (through itself or a view) and samples
// none of its image: the image is made after it (remakeImages), not before the range. A parallel encoder's passes are
// one pass, its write landing after the last of them; `t` as an attachment fuses with no pass. A write through another
// texture over the surface may sit in a pass fused with one that samples `t`, so it is not taken; nor is a range begun
// inside an encoder read past.
bool writtenFirst(RMCommandBuffer *cb, size_t from, size_t to, RMTexture *t)
{
    const RMCommandPlan &plan = cb->_plan;
    if (!RMCommandPlan::isEncoderBegin(plan.operation(from).op))
        return false;
    bool sampled = false, written = false;
    forEachEncoder(plan, from, to, [&](size_t begin, size_t end) {
        plan.forEachResource(begin, end, RMResourceKind::Texture, [&](const RMRangeResource &use) {
            sampled |= sampledImage(use) == t;
            written |= (use.access & raw::ResourceUse::Write) && rmStorageOwner((__bridge RMTexture *)use.object) == t;
        });
        return !sampled && (!written || (end < to && rmContinuesPass(cb, end)));
    });
    return written && !sampled;
}

}  // namespace

// The images of the converted textures the range samples, from their planes, before its passes, whose prologue's full
// barrier orders the copies before their reads. A texture bound where no draw's state reads it is not converted: the
// image is made when a draw samples it. Its samplers read it under its address map (Texture::addressMap): a sampler state
// through its header made here, a constexpr sampler through the one placed with it (Device::constexprMapped_).
bool RMRangeEncode::conversions()
{
    raw::Device &d = *d_;
    RMCommandBuffer *cb = cb_;
    const RMCommandPlan &plan = cb->_plan;
    std::vector<__unsafe_unretained RMTexture *> planar;
    std::vector<__unsafe_unretained RMSampler *> samplers;
    plan.forEachResource(from_, to_, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        __unsafe_unretained RMTexture *t = sampledImage(use);
        if (t && std::find(planar.begin(), planar.end(), t) == planar.end())
            planar.push_back(t);
    });
    if (planar.empty())
        return true;
    plan.forEachResource(from_, to_, RMResourceKind::Sampler, [&](const RMRangeResource &use) {
        samplers.push_back((__bridge RMSampler *)use.object);
    });
    // the samplers' headers under the planar textures' address maps (m2v's), which the draws' handles take
    bool placed = d.constexprMapsPlaced();
    for (__unsafe_unretained RMTexture *t : planar)
        for (__unsafe_unretained RMSampler *sm : samplers)
            placed = placed && (!t->_raw.addressMap || d.samplerVariant(sm->_raw, t->_raw.addressMap));
    if (!placed) {
        hit(cb, PredEncPlanarClamp, 0, 0, "no sampler header left in the device's table for an address map");
        failCommandBuffer(cb, @"no sampler header left in the device's table for a planar texture's address modes",
                          MTLCommandBufferErrorOutOfMemory);
        rc_->abandon();
        abandoned_ = true;
        return false;
    }
    plan.forEachResource(from_, to_, RMResourceKind::Texture, [&](const RMRangeResource &use) {
        for (__unsafe_unretained RMTexture *t : planar)
            if (writesPlanesOf(use, t) && std::find(rewritten_.begin(), rewritten_.end(), t) == rewritten_.end())
                rewritten_.push_back(t);
    });
    // an image converted already from its surface at the surface's seed now is not converted again: the work that
    // converted it went before on this channel (the decoder and ImageIO write a surface under a write lock, which
    // moves its seed). The new seeds take effect once this command buffer is submitted.
    std::vector<__unsafe_unretained RMTexture *> stale;
    for (__unsafe_unretained RMTexture *t : planar) {
        if (std::find(rewritten_.begin(), rewritten_.end(), t) != rewritten_.end() &&
            writtenFirst(cb, from_, to_, t))
            continue;
        const uint32_t seed = t->_surface ? IOSurfaceGetSeed(t->_surface) : 0;
        const uint64_t writes = t->_surface ? surfaceWrites(t->_surfaceWrites) : 0;
        const raw::Texture *luma = rc_->readTexture(t->_raw.luma);
        const uint64_t nativeGeneration = luma ? luma->nativeGeneration : 0;
        if (!t->_surface || !t->_imageCurrent || t->_imageSeed != seed || t->_imageWrites != writes ||
            t->_imageNativeGeneration != nativeGeneration) {
            stale.push_back(t);
            converted_.push_back({t, seed, writes, nativeGeneration});
#if SWITCH_FRAME_TIMELINE
            traceSurfaceImage(t, "converted", seed);
#endif
        }
    }
    return makeImages(stale);
}

// After the encoders that ended at `end`: the images of the converted textures they wrote the planes of, made again where
// later work of the range samples them. The encoders that ended are encoder_ and, after a parallel encoder's first pass,
// its others: a pass fused into the one before it has that one's attachments and writes nothing else. A range begun inside
// the encoder that ended names none, and every such texture is taken as written.
bool RMRangeEncode::remakeImages(size_t end)
{
    const RMCommandPlan &plan = cb_->_plan;
    std::vector<__unsafe_unretained RMTexture *> written;
    for (__unsafe_unretained RMTexture *t : rewritten_) {
        bool wrote = encoder_ == SIZE_MAX;
        if (!wrote)
            forEachEncoder(plan, encoder_, end, [&](size_t begin, size_t encoderEnd) {
                plan.forEachResource(begin, encoderEnd, RMResourceKind::Texture, [&](const RMRangeResource &use) {
                    wrote |= writesPlanesOf(use, t);
                });
                return !wrote;
            });
        if (wrote && sampledIn(plan, end, to_, t))
            written.push_back(t);
    }
    return makeImages(written);
}

// The images of `planar` drawn from their planes now, in the command stream: nothing waits for them on the CPU
bool RMRangeEncode::makeImages(const std::vector<__unsafe_unretained RMTexture *> &planar)
{
    raw::Device &d = *d_;
    raw::CommandBuffer *rc = rc_;
    // 4:4:4 code textures and one-plane 4:2:2 YCbCr textures' planes: two copy-engine remaps each
    bool copies = false;
    for (__unsafe_unretained RMTexture *t : planar)
        copies |= t->_raw.planar || t->_raw.onePlane;
    if (copies) {
        raw::BlitEncoder *conv = rc->blit();
        for (__unsafe_unretained RMTexture *t : planar) {
            if (t->_raw.planar)
                conv->convertPlanar(t->_raw);
            if (t->_raw.onePlane)
                conv->splitPlane(t->_raw);
        }
        conv->end();
    }
    // YCbCr textures' planes whose rows or offset are 16 bytes apart but not 32 (ImageIO's photo thumbnails, a width of
    // 360: rows of 368) are sampled through pool images (raw::Texture::sampled, createSurfaceTexture): filled from the
    // planes by the copy engine first, as refresh() fills a sampled surface's (r7u: without it the conversion read
    // whatever the pool held, magenta and green stripes of older images)
    raw::BlitEncoder *fill = nullptr;
    for (__unsafe_unretained RMTexture *t : planar)
        if (t->_raw.ycbcr)
            for (const raw::Texture *x : {t->_raw.luma, t->_raw.chroma})
                if (x && x->sampled && rc->readTexture(x) == x) {
                    if (!fill)
                        fill = rc->blit();
                    fill->copyTexels(*x, 0, 0, 0, *x->sampled, 0, 0, 0, x->width, x->height);
                }
    if (fill)
        fill->end();
    // YCbCr textures: a pass of the pack's conversion state each, into the image the draws sample. The image is the
    // texture's own block-linear colour target, so render() refuses the pass only for want of memory.
    for (__unsafe_unretained RMTexture *t : planar) {
        raw::Texture &x = t->_raw;
        if (!x.ycbcr)
            continue;
        raw::PassDesc p;
        p.color[0].texture = &x;
        p.color[0].load = raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        raw::RenderEncoder *conv = rc->render(p, why_);
        if (!conv) {
            rmPassWorkFails(cb_, ahead_, *why_, MTLCommandBufferErrorOutOfMemory);
            return fail(PredEncPassRender, *why_, x.width, x.height);
        }
        conv->setPipeline(d.ycbcrPipeline(x.ycbcr));
        conv->setBytes(2, 0, x.shape, sizeof x.shape);
        conv->setTexture(0, x.luma);
        conv->setTexture(1, x.chroma ? x.chroma : x.luma);  // (over plane 1: chroma codes 0, none read)
        conv->setViewport(0, 0, x.width, x.height, 0, 1);
        conv->setScissor(0, 0, x.width, x.height);
        if (!conv->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why_)) {
            rmOwnDrawFails(cb_, ahead_, *why_, rc->refusal);
            return refused();
        }
        conv->end();
    }
    return true;
}
