// SPDX-License-Identifier: MIT
// Two-plane YCbCr textures as render targets (raw_ycbcr_store.h): Metal holds a pass's texels as halves and stores each
// texel's luma code and each chroma block's rounded mean code (code formats), or m2v's matrix rule (converted ones). The raw path
// renders the pass into a memoryless RGBA32Float stand-in (its pipelines are made for that format in the attachment's place);
// the pack's store states then draw the luma plane and the chroma blocks from the pass's backing of it, every code worked out in
// the program (pack/ycbcr_store.metal). The 2D engine's 2:1 bilinear halving stores a tie rounded down where Metal's mean rounds
// it up (24.9% of 4:2:0 and 48.1% of 4:2:2 chroma bytes off on the device), so the blocks are a draw too. A pass that loads the
// texture renders into a stand-in a pass of the load state filled first (loadYcbcrStandIn), and stores only the tiles its draws
// reach (raw_ycbcr_load.h). R10Unorm_PACKED and RG10Unorm_PACKED planes (raw_packed_plane.h) render the same way, into an
// R32Float or RG32Float stand-in, through one store state into their words, which gives each loaded code back.
#import "RawMetalInternal.h"
#import "RawMetalReflection.h"
#include "raw_format_standin.h"
#include "raw_overlap_rows.h"
#include "raw_packed_plane.h"
#include "raw_planar_target.h"
#include "raw_standin_blend.h"
#include "raw_ycbcr_load.h"
#include "raw_ycbcr_store.h"

static raw::Pipeline *packState(const char *name)
{
    return name ? gRM->raw->ownPipeline(name) : nullptr;
}

static raw::Pipeline *loadState(const raw::Texture &t)
{
    return t.ycbcrStore->plane ? gRM->raw->ycbcrLoadPipeline(t.ycbcrStore->plane)
                               : packState(raw::ycbcrLoadState(t.ycbcr));
}

// A one-plane 4:2:2 target whose pairs' rows overlap: its luma and chroma images are stored, then its pair bytes drawn from
// them (raw_overlap_rows.h)
static bool pairsOverlap(const raw::Texture &t)
{
    return t.onePlane && t.planes.lumaPitch < 4 * t.chroma->width;
}

// Where a plane's rows overlap, the pool image its rows are stored into and the pack's state that draws its bytes from it
static bool overlapReady(const raw::Texture *plane)
{
    return !plane || !raw::overlapsRows(*plane) || (plane->sampled && packState(raw::overlapState(plane->format)));
}

static bool loadable(const raw::Texture &t)
{
    const raw::YcbcrStoreShape &s = *t.ycbcrStore;
    return loadState(t) && (s.plane || (packState(raw::ycbcrMaskedStoreState(false, s)) &&
                                        packState(raw::ycbcrMaskedStoreState(true, s))));
}

void makeYcbcrTarget(raw::Texture &t, uint32_t pixelFormat, const raw::YcbcrStoreShape &store)
{
    raw::Device &d = *gRM->raw;
    const auto rendered = [](const raw::Texture *x) {
        return x && (x->renderable || x->shadowTarget);
    };
    raw::Texture *standIn = new raw::Texture;
    // a packed plane's words are its one plane, as YCBCR10_444_1P's are (its luma header's); a texture over plane 1 stores
    // its luma codes alone
    const bool chroma = !store.plane && !store.words && t.chroma;
    const bool overlaps = t.onePlane ? !pairsOverlap(t) || packState(raw::overlapState(10))
                                     : overlapReady(t.luma) && (!chroma || overlapReady(t.chroma));
    // RGB8_444_2P's pipelines render into an RGBA16Float stand-in, as the planar path's targets do (RawOffline.mm)
    const uint32_t standInFormat =
        pixelFormat == 542 ? raw::kPlanarStandInFormat : raw::ycbcrStandInFormat(pixelFormat);
    if (!rendered(t.luma) || (chroma && !rendered(t.chroma)) || !overlaps || !d.ycbcrStorePipeline(false, store) ||
        (chroma && !d.ycbcrStorePipeline(true, store)) ||
        !d.createMemoryless(t.width, t.height, standInFormat, *standIn)) {
        delete standIn;
        SWITCH_REFUSE_DETAIL("raw", "ycbcr.target", pixelFormat,
                             "no stand-in or store state for a %ux%u format %u render target: its passes fail their "
                             "command buffers (enc.planarrt)",
                             t.width, t.height, pixelFormat);
        return;
    }
    t.planarStandIn = standIn;
    t.ycbcrStore = new raw::YcbcrStoreShape(store);
}

// A target without a stand-in is one made without render-target usage (Metal's validation stops the pass) or one whose
// stand-in or store states the device could not make; a loaded one draws the pack's own load and tile-masked store states
const char *ycbcrTargetPassRefusal(const raw::Texture &t, MTLRenderPassColorAttachmentDescriptor *a)
{
    const char *why = !t.planarStandIn || !t.ycbcrStore   ? "a YCbCr texture with no stand-in as a render target"
                      : a.loadAction != MTLLoadActionLoad ? nullptr
                      : loadable(t) ? nullptr
                                    : "a YCbCr target loaded: the pack has no load or tile-masked store state for it";
    if (why)
        SWITCH_REFUSE_DETAIL("raw", "pass.ycbcr", a.loadAction, "%ux%u format %u: %s: the command buffer fails",
                             t.width, t.height, uint32_t(a.texture.pixelFormat), why);
    return why;
}

int storeYcbcrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, const raw::Texture &standIn, const char **why,
                     const raw::PassReach *loaded)
{
    raw::Device &d = *gRM->raw;
    const bool masked = loaded && !t.ycbcrStore->plane;
    if (masked && !loaded->mask)
        return 0;
    for (const bool chroma : {false, true}) {
        if (chroma && (!t.chroma || t.ycbcrStore->words))
            continue;
        raw::Texture *plane = chroma ? t.chroma : t.luma;
        const bool overlap = !t.onePlane && raw::overlapsRows(*plane);
        raw::PassDesc p;
        p.color[0].texture = overlap ? plane->sampled : plane;
        p.color[0].load = masked ? raw::ColorAttachment::Load : raw::ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        raw::RenderEncoder *e = rc->render(p);
        if (!e) {
            *why = "a YCbCr texture's plane the raw path cannot render";
            return 1;
        }
        e->setPipeline(masked ? packState(raw::ycbcrMaskedStoreState(chroma, *t.ycbcrStore))
                              : d.ycbcrStorePipeline(chroma, *t.ycbcrStore));
        e->setTexture(0, &standIn);
        e->setBytes(2, 0, t.ycbcrStore, sizeof *t.ycbcrStore);
        if (masked) {
            const raw::YcbcrStoreTiles tiles{loaded->tilesX, loaded->tilesY};
            e->setTexture(1, loaded->mask);
            e->setBytes(2, raw::kYcbcrTilesSlot, &tiles, sizeof tiles);
            if (t.ycbcrStore->packed)
                e->setTexture(2, overlap ? plane->sampled : plane);
        }
        if (t.ycbcrStore->srgb) {
            const raw::Bo &table = d.srgbEncodeTable();
            e->setBytesAt(2, raw::kSrgbEncodeSlot, table.va, uint32_t(table.bytes), table.cpu);
        }
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return 2;
        e->end();
        if (overlap)
            if (const int failed = raw::storeOverlapRows(
                    *rc, packState(raw::overlapState(plane->format)), plane->format, plane->va, *plane,
                    *plane->sampled, nullptr, raw::overlapPlaneRows(*plane, chroma, *t.ycbcrStore),
                    masked ? loaded : nullptr, why))
                return failed;
    }
    if (pairsOverlap(t)) {
        if (const int failed =
                raw::storeOverlapRows(*rc, packState(raw::overlapState(10)), 10, t.planes.luma, t, *t.luma, t.chroma,
                                      raw::overlapPairsOf(t), masked ? loaded : nullptr, why))
            return failed;
    } else if (t.onePlane) {
        raw::BlitEncoder *merge = rc->blit();
        merge->mergePlane(t);
        merge->end();
    }
    static std::atomic<bool> said{false}, saidPlane{false};
    if (t.ycbcrStore->plane) {
        if (!saidPlane.exchange(true, std::memory_order_relaxed))
            os_log(
                rawLog(),
                "raw: packed 10-bit plane stored on the GPU (first: %ux%u, %u codes a texel): pass into a memoryless "
                "stand-in, words drawn by the pack's store state",
                t.width, t.height, t.ycbcrStore->plane);
    } else if (!said.exchange(true, std::memory_order_relaxed)) {
        os_log(rawLog(),
               "raw: YCbCr target stored on the GPU (first: %ux%u, chroma %ux%u, %u-bit %{public}s): pass into a "
               "memoryless stand-in, luma and chroma blocks drawn by the pack's store states",
               t.width, t.height, t.chroma ? t.chroma->width : 0, t.chroma ? t.chroma->height : 0,
               t.ycbcrStore->max > 255 ? 10u : 8u,
               t.ycbcrStore->srgb        ? "sRGB"
               : t.ycbcrStore->converted ? "converted"
                                         : "codes");
    }
    return 0;
}

// What the pass loads into a block-linear stand-in of the command buffer's, which the pass renders into in place of the memoryless
// one: each read's tile value (raw_ycbcr_load.h), or a packed 10-bit plane's codes over 1023, which its store gives back
// (raw_packed_plane.h packedPlaneLoad). The planes' sampled shadows are refreshed first, as for a conversion.
int loadYcbcrStandIn(raw::CommandBuffer *rc, const raw::Texture &t, uint32_t j, raw::Texture *&loaded, const char **why)
{
    raw::BlitEncoder *fill = nullptr;
    // a one-plane 4:2:2 target's luma and chroma images hold its pairs before its load reads them and its tile-masked store
    // keeps the tiles the pass does not reach
    if (t.onePlane) {
        fill = rc->blit();
        fill->splitPlane(t);
    }
    for (const raw::Texture *plane : {t.luma, t.chroma})
        if (plane && plane->sampled && rc->readTexture(plane) == plane) {
            if (!fill)
                fill = rc->blit();
            fill->copyTexels(*plane, 0, 0, 0, *plane->sampled, 0, 0, 0, plane->width, plane->height);
        }
    if (fill)
        fill->end();
    const uint32_t tag = (t.ycbcrStore->plane ? raw::kPackedLoadTag : raw::kYcbcrLoadTag) + j;
    loaded = rc->turnScratch(t.planarStandIn->format, t.width, t.height, tag);
    if (!loaded) {
        *why = "no stand-in for a loaded YCbCr target";
        return 1;
    }
    raw::PassDesc p;
    p.color[0].texture = loaded;
    p.color[0].load = raw::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    raw::RenderEncoder *e = rc->render(p);
    if (!e) {
        *why = "a loaded YCbCr target's stand-in the raw path cannot render";
        return 1;
    }
    e->setPipeline(loadState(t));
    e->setBytes(2, 0, t.shape, sizeof t.shape);
    e->setTexture(0, t.luma);
    if (!t.ycbcrStore->plane)
        e->setTexture(1, t.chroma ? t.chroma : t.luma);
    e->setViewport(0, 0, t.width, t.height, 0, 1);
    e->setScissor(0, 0, t.width, t.height);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return 2;
    e->end();
    return 0;
}

// The tiles a pass's draws reach (raw_reach.h), a YCbCr target cleared and stored counting as any other attachment does
bool reachOfLoadedTargets(const RMPass &pass, const raw::PassDesc &p, raw::DrawReplay &draws, raw::CommandBuffer &rc,
                          raw::PassReach &out, const char **why)
{
    raw::PassDesc q = p;
    for (uint32_t k = 0; k < p.count && k < 8; k++)
        if (__unsafe_unretained RMTexture *t = (RMTexture *)pass.color[k].texture; t && t->_raw.planarStandIn)
            q.color[k].texture = t->_raw.luma;
    return rc.reach(q, p.width, p.height, draws, out, why);
}

void rmAlphaOneBlend(MTLRenderPipelineColorAttachmentDescriptor *a)
{
    a.sourceRGBBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(a.sourceRGBBlendFactor), true));
    a.destinationRGBBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(a.destinationRGBBlendFactor), true));
    a.sourceAlphaBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(a.sourceAlphaBlendFactor), false));
    a.destinationAlphaBlendFactor = MTLBlendFactor(raw::alphaOneFactor(uint32_t(a.destinationAlphaBlendFactor), false));
}

uint32_t standInTargetFormat(MTLRenderPipelineDescriptor *d)
{
    for (NSUInteger c = 0; c < 8; c++)
        if (const uint32_t format = uint32_t(d.colorAttachments[c].pixelFormat);
            raw::ycbcrRenderedFormat(format) || raw::formatStandIn(format))
            return format;
    return 0;
}

@implementation RMDevice (YcbcrTargets)

// A state of an attachment the raw path renders through a stand-in, YCbCr (raw_ycbcr_store.h) or of a format the GM20B renders
// none of (raw_format_standin.h), made for its stand-in format, blended and channel-masked ones too: the stand-in holds the
// pass's texels, the channels a mask keeps as loaded or cleared, and a format without alpha blends by a destination alpha of
// 1 (raw_standin_blend.h). An M4 Max blends YCbCr in halves of its own rounding, so a blended store is within one code of
// Metal's (raw/test/ycbcrblendoracle.mm); a masked one is Metal's. A format ycbcrTargetRefusal names has no target texture
// (planYcbcr refuses its render-target usage), so its state draws no pass, as Metal makes it all the same. The archives a
// creation under FailOnBinaryArchiveMiss lists answer for the app's state, not its stand-in.
- (id<MTLRenderPipelineState>)standInTargetState:(MTLRenderPipelineDescriptor *)d
                                         options:(MTLPipelineOption)o
                                      reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                           error:(NSError **)error
{
    if (const char *why = rmUnsupportedRenderState(d))
        return rmRefuseUnsupported(why, error);
    if (rmRenderArchiveMissed(d, o, error))
        return nil;
    o &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    MTLRenderPipelineDescriptor *made = [d copy];
    for (NSUInteger c = 0; c < 8; c++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[c];
        const uint32_t format = uint32_t(a.pixelFormat);
        if (const uint32_t standIn = raw::formatStandIn(format)) {
            made.colorAttachments[c].pixelFormat = MTLPixelFormat(standIn);
            if (!raw::formatHoldsAlpha(format))
                rmAlphaOneBlend(made.colorAttachments[c]);
            continue;
        }
        if (!raw::ycbcrRenderedFormat(format))
            continue;
        MTLRenderPipelineColorAttachmentDescriptor *standIn = made.colorAttachments[c];
        standIn.pixelFormat = MTLPixelFormat(raw::ycbcrStandInFormat(format));
        rmAlphaOneBlend(standIn);
    }
    RMPipeline *p = rmPipelineFor(self, made, o, error);
    if (!p)
        return nil;
    if (!p->_raw) {
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{
                                         NSLocalizedDescriptionKey : @"the raw path made no state for a "
                                                                     @"stand-in's target, and m2v renders none"
                                     }];
        return nil;
    }
    if (reflection)
        *reflection = rmRenderReflection(p, made, o);
    p->_twinOptions = o;
    p->_twinLater = true;
    if (error)
        *error = nil;
    return (id<MTLRenderPipelineState>)p;
}

@end
