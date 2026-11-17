// SPDX-License-Identifier: MIT
// See raw_gpu.h (RenderEncoder::prepare: a draw's state, set 0 and barriers).
#include "raw_lock.h"
#include "raw_nonfinite.h"
#include "raw_clear_box.h"
#include "raw_icb.h"
#include "raw_dynamic_push.h"
#include "raw_blend_color.h"
#include "raw_gpu.h"
#include "raw_layers.h"
#include "raw_kit.h"
#include "raw_mme.h"
#include "raw_cbuf.h"
#include "raw_residual.h"
#include "raw_multisample.h"
#include "raw_rotated_pass.h"
#include "raw_fragment_tests.h"
#include "raw_shader_tests.h"
#include "raw_shader_test_push.h"
#include "raw_srgb_table.h"
#include "raw_storage.h"
#include "raw_tiled_cache.h"
#include "raw_view.h"
#include "raw_xr_pass.h"
#include "raw_written_textures.h"

#include <mach/mach_time.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "raw_gpu_internal.h"

namespace raw {

namespace {
// The viewport's words (scale X Y Z, offset X Y Z as floats' bits, then each clip's x | width << 16) as the kit's state
switch_gm20b_viewport_state viewportState(const uint32_t w[8])
{
    switch_gm20b_viewport_state v = {};
    memcpy(v.scale, w, sizeof v.scale);
    memcpy(v.offset, w + 3, sizeof v.offset);
    v.clip_x = uint16_t(w[6]);
    v.clip_width = uint16_t(w[6] >> 16);
    v.clip_y = uint16_t(w[7]);
    v.clip_height = uint16_t(w[7] >> 16);
    return v;
}

constexpr uint32_t kRtLayerFromShader = NVDEF(NVB197, SET_RT_LAYER, CONTROL, GEOMETRY_SHADER_SELECTS_LAYER);
}  // namespace

uint32_t nvTopology(uint32_t mtl)
{
    return icb::topology(mtl);
}

void CommandBuffer::clean(const void *cpu, uint64_t bytes, bool invalidate)
{
    dirty_.add(cpu, bytes, invalidate);
}

// Viewports and scissors 1 .. count - 1 as NVK writes viewport j (nvk_emit_viewport, nvk_emit_scissor), through a turned
// pass's transform and its clip-space swizzle at each index (index 0's is the pass's prologue's)
void RenderEncoder::emitViewports(Push &p)
{
    for (uint32_t dirty = views_.dirtyViewports; dirty; dirty &= dirty - 1) {
        const uint32_t j = uint32_t(__builtin_ctz(dirty));
        switch_gm20b_viewport_state v = viewportState(views_.viewport[j]);
        turnViewport(rot_, v);
        kit(p, switch_gm20b_viewport, j, &v);
        if (rot_.turned() && !(views_.swizzled >> j & 1)) {
            kit(p, switch_gm20b_viewport_swizzle, j, rot_.swizzle());
            views_.swizzled |= 1u << j;
        }
    }
    for (uint32_t dirty = views_.dirtyScissors; dirty; dirty &= dirty - 1) {
        const uint32_t j = uint32_t(__builtin_ctz(dirty));
        const ScissorBox &b = views_.scissor[j];
        const Rect s = planeRect(b.x0, b.y0, b.x1, b.y1);
        kit(p, switch_gm20b_scissor, j, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
    }
    views_.dirtyViewports = views_.dirtyScissors = 0;
}

// The colour targets the pass binds: through its last attachment (CommandBuffer::render's targets)
uint32_t RenderEncoder::colorTargets() const
{
    uint32_t count = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (pass_.color[j].texture)
            count = j + 1;
    return count;
}

#if SWITCH_RAW_DIAGNOSTICS
static void probeUniforms(Pipeline &pl, uint32_t stage, uint32_t slot, const uint8_t *bytes, uint64_t length)
{
    const NonFiniteUniforms found = nonFiniteUniforms(bytes, length);
    if (!found.words && (!found.halves || pl.uniformHalvesReported))
        return;
    char hex[3 * 32 + 1] = "";
    const uint64_t from = found.first & ~uint64_t(15);
    for (uint64_t at = from, n = 0; at < length && n < 32; ++at, ++n)
        snprintf(hex + 3 * n, 4, "%02x ", bytes[at]);
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
           "raw: uniform probe %{public}s stage %u slot %u: %u non-finite floats and %u halves in %llu bytes, first at "
           "+%llu; "
           "+%llu: %{public}s(mach %llu)",
           pl.uniformProbe, stage, slot, found.words, found.halves, (unsigned long long)length,
           (unsigned long long)found.first, (unsigned long long)from, hex, (unsigned long long)mach_absolute_time());
    if (found.words)
        pl.uniformProbe = nullptr;
    else
        pl.uniformHalvesReported = true;
}
#endif

// Stage `k`'s (0 vertex, 1 fragment) root table for this draw, in the upload ring: m2v's {null, sink, an entry per buffer slot
// it reaches through the table} over the identity resident set (raw_heap_tables.h). A buffer reached so may be written.
uint64_t RenderEncoder::rootTable(const PackPipeline &r, uint32_t k, bool writes)
{
    Device &d = *dev_;
    const uint32_t slots = r.rootSlots[k], entries = drawRootEntries(slots);
    RootSlot slot[32];
    for (uint32_t n = 0; kFirstSlotRoot + n < entries; n++) {
        const Bound &b = bufs_[k + 1][n];
        slot[n] = {};
        if (!(slots >> n & 1) || !b.va)
            continue;
        if (b.buffer) {
            cb_->use(*b.buffer, true, writes);
            slot[n] = {b.buffer->va, b.buffer->length, b.offset};
        } else {
            cb_->useAddress(b.va, b.size, true);
            slot[n] = {b.va, b.size, 0};
        }
    }
    uint64_t va = 0;
    auto *table = reinterpret_cast<RootEntry *>(d.alloc(d.upload_, entries * sizeof(RootEntry), 256, va));
    writeDrawRoots(table, va, slots, slot, d.heapTables_.sinkVa(), d.heapTables_.identityVa());
    RAW_DIAGNOSTIC(d.stats.uploadBytes += entries * sizeof(RootEntry));
    return va;
}

// Set-0 entry `e` of `pl` into `set`, its buffer's bind residual into `prepared`; false: the draw is refused (`why`)
__attribute__((always_inline)) inline bool RenderEncoder::writeSetEntry(Pipeline *pl, const PackEntry &e, uint32_t *set,
                                                                        SetResiduals &prepared, const char **why)
{
    Device &d = *dev_;
    const PackPipeline &r = *pl->rec;
    uint32_t *at = set + e.offset / 4;
    if (e.kind == EntrySpillTable)
        return true;
    if (e.kind == EntryColorTable) {
        at[0] = uint32_t(d.colorTable_.va);
        at[1] = uint32_t(d.colorTable_.va >> 32);
        at[2] = kColorTableBytes;
        at[3] = 0;
        return true;
    }
    if (e.kind == EntryTextureTable || e.kind == EntrySamplerTable) {
        const bool textures = e.kind == EntryTextureTable;
        const uint64_t va = textures ? d.heapTables_.textureTableVa() : d.heapTables_.samplerTableVa();
        at[0] = uint32_t(va);
        at[1] = uint32_t(va >> 32);
        at[2] = textures ? d.heapTables_.textureTableBytes() : d.heapTables_.samplerTableBytes();
        at[3] = 0;
        return true;
    }
    if (e.kind == EntryUbo || e.kind == EntryTessBuffer) {
        const Bound b = e.kind == EntryUbo ? bufs_[e.stage][e.slot] : tessBuffer(e.slot);
        if (bufferStorage(e))
            cb_->bufferWrites_.note(b.va, b.size);
        if (b.buffer)
            cb_->use(*b.buffer, bufferStorage(e));
        else if (!b.named || bufferStorage(e))
            cb_->useAddress(b.va, b.size, bufferStorage(e));
        BufferDescriptor desc;
        if (!bufferDescriptor(e, b.va, b.size, desc)) {
            *why = bufferStorage(e) ? "a storage buffer bound at an offset that is not a multiple of 16, with no "
                                      "residual push word"
                                    : "a uniform buffer bound at an offset that is not a multiple of 256, with no "
                                      "residual push word";
            cb_->refusal = {RefUboOffset, uint32_t(e.stage) << 8 | e.slot, desc.residual,
                            uint32_t(std::min<uint64_t>(b.offset, ~0u))};
            return false;
        }
        // a word that held a residual takes the slot's next one even where it is 0: the set keeps its
        // words from the draws before
        if (const int word = bufferResidualWord(e); spilledWord(word)) {
            prepared.spilled[e.stage == 1 ? 0 : 1].set(word, desc.residual);
        } else if (word >= 0 && (desc.residual || (prepared.residualMask >> word & 1))) {
            prepared.residuals[word] = desc.residual;
            prepared.residualMask |= 1u << word;
        }
        if (b.buffer && b.buffer->hostVisible && !b.buffer->wc)
            cb_->clean(b.buffer->cpu + b.offset, std::min<uint64_t>(desc.range - desc.residual, b.size));
#if SWITCH_RAW_DIAGNOSTICS
        if (pl->uniformProbe && b.cpu)
            probeUniforms(*pl, e.stage, e.slot, b.cpu, std::min<uint64_t>(desc.range - desc.residual, b.size));
#endif
        at[0] = uint32_t(desc.base);
        at[1] = uint32_t(desc.base >> 32);
        at[2] = b.va ? desc.range : 0;
        at[3] = 0;
        return true;
    }
    RAW_ENCODE_SPAN(Textures);
    uint32_t tic = 0, tsc = 0;
    if (e.kind == EntryStencilTest && shadowStencil(r.shaderTests) && !(r.shaderTests.flags & kShaderTestStencil)) {
        tic = 0;  // a shader-side stencil's program in a pass with no stencil tests none (m2v's neutral)
    } else if (e.kind == EntryDepthTest || e.kind == EntryStencilTest) {
        const bool depth = e.kind == EntryDepthTest;
        const Texture *t = depth ? shaderDepth_ : shaderStencil_;
        const uint32_t view = depth ? r.shaderTests.depthTic : r.shaderTests.stencilTic;
        if (t && layeredRecord(r.arrayLengthWord) && pass_.layers > 1) {
            AttachmentView layers;
            layers.word0 = view;
            layers.layers = pass_.layers;
            layers.arrayed = true;
            tic = texelBytes(view) == t->bpp ? passViewTic(*t, layers, why) : 0;
        } else if (t && texelBytes(view) == t->bpp) {
            tic = passViewTic(*t, view);
        }
        if (!t || t->samples != samples_ || !tic) {
            *why = "an unavailable native shader depth/stencil view";
            cb_->refusal = {RefDepthStencil, e.kind, t ? t->format : 0, samples_};
            return false;
        }
        cb_->use(*t, true);
    } else if (e.kind == EntryTexture) {
        const Texture *t = boundTexture(e);
        if (t && !t->tic) {
            *why = "a texture the raw path cannot sample";
            cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
            return false;
        }
        // a two-plane texture's header pair where the program reads it so, else its stand-in
        const bool xrSlot = e.stage == 2 && xrPairSlot(pl->xrSlots, e.slot);
        const bool pair = t && t->xrPlanes && xrSlot;
        if (!pair && xrSlot) {
            *why = "a state that reads a two-plane texture's header pair drawn without one";
            cb_->refusal = {RefNoTic, e.slot, t ? t->format : 0, t ? t->width : 0};
            return false;
        }
        const bool written = handleStage(e) == 1 && writtenSlot(pl->writtenSlots, e.slot);
        const Texture *image = written && t && t->xrPlanes ? pass_.writtenImage(*t) : nullptr;
        if (written && t && t->xrPlanes && !image) {
            *why = "a written RGB10A8_2P_XR10 texture held in its two planes with no image";
            cb_->refusal = {RefWrittenPlanes, e.slot};
            return false;
        }
        if (written && t && t->format != pl->rec->writtenFormat[e.slot]) {
            *why = "a written texture of another format than its program stores";
            cb_->refusal = {RefWrittenTexture, e.slot, t->format, pl->rec->writtenFormat[e.slot]};
            return false;
        }
        // A written texture is stored through tic: its own memory, or the sampled shadow that stands for
        // rows its header cannot address, which the frontend copies back after the pass; its stand-in is
        // drawn again from the codes before work after the pass samples it (RawMetalEncode.mm)
        if (pair) {
            cb_->use(*t->luma, false);
            cb_->use(*t->chroma, false);
            RAW_DIAGNOSTIC(dev_->stats.xrPairBinds++);
        } else if (image) {
            cb_->use(*image, true);
        } else if (t && written) {
            cb_->use(t->sampled ? *t->sampled : *t, true);
        } else if (t) {
            cb_->use(t->standIn ? *t->standIn : t->sampled ? *t->sampled : *t, false);
        }
        tic = !t ? 0 : image ? image->tic : pair ? t->tic : t->standIn && !written ? t->standIn->tic : t->tic;
    } else if (e.kind == EntryFetch) {
        const ColorAttachment &a = pass_.color[e.slot];
        const Texture *t = a.texture;
        const bool storage = t && (pl->storageBits >> e.slot & 1);
        if (storage && storageViewMismatch(texelBytes(pl->storageTic[e.slot]), pl->rec->storageSamples, *t,
                                           dev_->storageCompressedOk()) != StorageFallback::None) {
            *why = "an incompatible framebuffer storage view";
            cb_->refusal = {RefStorageView, e.slot, t->format, t->width};
            return false;
        }
        if (t) {
            cb_->use(*t, pl->storageWriteBits >> e.slot & 1);
            tic = fetchTic(a, storage ? pl->storageTic[e.slot] : 0, layeredRecord(pl->rec->arrayLengthWord), why);
            if (!tic) {
                cb_->refusal = {storage ? RefStorageView : RefNoTic, e.slot, t->format, t->width};
                return false;
            }
        }
    } else if (e.kind == EntryXrShadow) {
        const Texture *shadow = pass_.xrShadow[e.slot];
        if (!shadow || !shadow->tic) {
            *why = "an extended-range shadow's variant drawn in a pass that holds no shadow of the colour";
            cb_->refusal = {RefStorageView, e.slot, 0, 0};
            return false;
        }
        cb_->use(*shadow, true);
        tic = shadow->tic;
    } else if (e.kind == EntryTexture3d) {
        // a texture of another type there is no binding of the argument: m2v writes its neutral, raw an empty handle
        const Texture *bound = boundTexture(e);
        const Texture *t = bound && bound->type == fx::kTexThreeD ? bound : nullptr;
        if (t && !t->tic) {
            *why = "a texture the raw path cannot sample";
            cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
            return false;
        }
        const bool written = handleStage(e) == 1 && writtenSlot(pl->writtenSlots, e.slot);
        if (written && t && t->format != pl->rec->writtenFormat[e.slot]) {
            *why = "a written texture of another format than its program stores";
            cb_->refusal = {RefWrittenTexture, e.slot, t->format, pl->rec->writtenFormat[e.slot]};
            return false;
        }
        if (t)
            cb_->use(*t, written);
        tic = t ? t->tic : 0;
    }
    const Texture *mapped = e.kind == EntryTexture ? boundTexture(e) : nullptr;
    const uint8_t map = mapped ? mapped->addressMap : 0;
    if (e.samplerKind == SamplerSlot) {
        const Sampler *s = boundSampler(e);
        const bool compares = entryCompares(e);
        if (s && map && compares) {
            *why = "a planar texture sampled by comparison";
            return false;
        }
        tsc = s          ? (map        ? s->mapped[map]
                            : compares ? s->compareTsc
                                       : borderedTsc(*s, mapped))
              : compares ? d.neutralCompareTsc_
                         : d.neutralTsc_;
        if (s && !tsc) {
            *why = map ? "a planar texture's sampler without its address map"
                       : "a sampler without its opaque border's header for the texture";
            return false;
        }
    } else if (e.samplerKind == SamplerConstexpr) {
        tsc = map ? d.constexprMapped_[e.sampler][map] : d.constexprTsc_[e.sampler];
        if (!tsc) {
            *why = "a planar texture's constexpr sampler without its address map";
            return false;
        }
    }
    at[0] = tic | tsc << 20;
    return true;
}

bool RenderEncoder::prepare(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t vFirst, uint32_t vCount,
                            bool indexed, const char **why)
{
    RAW_ENCODE_SPAN(Prepare);
    if (pl->strideTwin && bindStridePastStream(*pl->rec))
        pl = pl->strideTwin;
    if (tessellates(*pl->rec) != (primitive == icb::kPatchPrimitive)) {
        *why = primitive == icb::kPatchPrimitive ? "patches drawn with a state of no post-tessellation vertex function"
                                                 : "primitives drawn with a tessellation state";
        cb_->refusal = {RefTessellationDraw, primitive, tessellates(*pl->rec)};
        return false;
    }
    if (const char *refused = drawnWordsRefusal(*pl->rec, cb_->refusal)) {
        *why = refused;
        return false;
    }
    // A record of a vertex function that chooses its slice writes Layer below the pass's length (arrayLengthWord) and
    // one that chooses its viewport ViewportIndex below the encoder's count (viewportCountWord); a record of one layer
    // draws layer 0, and a record made before the viewport word viewport 0
    if (const char *missing = layeredDrawMissing(pl->rec->arrayLengthWord, pass_.layers, kLayerDropped)) {
        *why = missing;
        cb_->refusal = {RefLayerDropped, pass_.layers};
        return false;
    }
    if (views_.viewports > 1 && pl->viewportIndex && !pl->rec->viewportCountWord) {
        *why = "a vertex function choosing among several viewports: its record draws viewport 0";
        cb_->refusal = {RefViewports, views_.viewports, 0, 0};
        return false;
    }
    const uint32_t count = vCount;
    const bool firstDraw = !drew_;
    drew_ = true;
    ++passDraws_;
    readBits_ |= pl->readBits | pl->fetchBits;
    // the pixels it may write: its scissor within its viewport's pixels (m2v vk_scissors, viewport_pixels), over each
    // viewport its vertex function may choose
    uint32_t x[2], y[2];
    viewportPixels(vp_, {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]}, areaW_, areaH_, x, y);
    for (uint32_t j = 1; pl->viewportIndex && j < views_.viewports; j++) {
        uint32_t xj[2], yj[2];
        viewportPixels(views_.vp[j], j < views_.scissors ? views_.scissor[j] : ScissorBox{0, 0, areaW_, areaH_}, areaW_,
                       areaH_, xj, yj);
        if (xj[1] <= xj[0] || yj[1] <= yj[0])
            continue;
        if (x[1] <= x[0] || y[1] <= y[0]) {
            x[0] = xj[0], x[1] = xj[1], y[0] = yj[0], y[1] = yj[1];
            continue;
        }
        x[0] = std::min(x[0], xj[0]), x[1] = std::max(x[1], xj[1]);
        y[0] = std::min(y[0], yj[0]), y[1] = std::max(y[1], yj[1]);
    }
    Pipeline *prepared = dev_->variant(*pl, bindingFacts(*pl) | (rot_.turned() ? AssumeTurned : 0u), selectorId_);
    if (prepared->selectorVariant)
        pl = prepared;
    RAW_ENCODE_BEGIN(geometrySpan, Geometry);
    // only a pass's first draw is bounded by its vertices, for the cover test below (its vertex bytes the recording's
    // cached copy, RMCommandBuffer::_shadow); every other decision takes the draw's pixel limit
    DrawGeometry g;
    if (!firstDraw || (pl->storageBits && !(pl->fetchBits & ~pl->storageBits)))
        g.limit = {x[0], y[0], x[1], y[1]};
    else
        g = geometry(*pl, primitive, first, indexed ? drawIndexCount_ : count, {x[0], y[0], x[1], y[1]});
    auto geometryAnalysis = geometryCache_.analysis(g, bindingRevision_, cb_->geometryBudget_, &cb_->geometryPool_);
    RAW_ENCODE_BEGIN(variantSpan, Variant);
    // a turned draw of a turned variant's state takes the variant over the generic program's split twin
    Pipeline *twin = indirectGeometry_ || (rot_.turned() && pl->turnedVariant) ? pl : splitTwin(pl, geometryAnalysis);
    const Pipeline *launchOnly = nullptr;
    bool generic = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (twin->splitOf && cb_->splitProbe_ == SplitProbe::LaunchOnly)
        launchOnly = twin;
    generic = twin->splitOf && (cb_->splitProbe_ == SplitProbe::Generic || launchOnly);
#endif
    if (!generic)
        pl = twin;
    // a variant has no split of its own: a draw large enough for the generic program's split twin keeps the twin
    if (rot_.turned() && pl->turnedSource) {
        Pipeline *turned = __atomic_load_n(&pl->turnedTwin, __ATOMIC_ACQUIRE);
        if (!turned && !(turned = dev_->makeTurned(*pl))) {
            *why = rotatedRefusalText(RotatedRefusal::Variant);
            cb_->refusal = {RefRotated, uint32_t(RotatedRefusal::Variant), rot_.turn};
            return false;
        }
        pl = turned;
    } else if (!pl->splitOf && !generic)
        pl = prepared;
    RAW_ENCODE_END(variantSpan);
    if (!whole_) {
        if (x[1] > x[0] && y[1] > y[0]) {
            const std::array<uint32_t, 4> r = {x[0], y[0], x[1], y[1]};
            if (std::find(rects_.begin(), rects_.end(), r) == rects_.end()) {
                rects_.push_back(r);
                growReached(reached_, {r[0], r[1], r[2], r[3]});
            }
            if (rects_.size() > 1024) {
                whole_ = true;
                rects_.clear();
            }
        }
    }
    RAW_ENCODE_END(geometrySpan);
    const Texture *zeta = pass_.depthStencil.texture;
    if (compatible_ != pl) {
        for (uint32_t j = 0; j < 8; j++) {  // the pass's attachments are the formats the state was made for
            const Texture *t = j < pass_.count ? pass_.color[j].texture : nullptr;
            const uint32_t f = t ? t->format : 0;
            if (f != pl->colorFormats[j] && !(t && pl->colorFormats[j] == 0) &&
                !(targetOrdersChannels(pl->colorFormats[j], f) && !(pl->storageBits >> j & 1))) {
                *why = "a state whose color formats differ from the pass's: the draw skipped";
                cb_->skipped = {RefColorFormats, j, f, pl->colorFormats[j]};
                cb_->skips++;
                skipDraw_ = true;
                return false;
            }
        }
        if (!fragmentTestsMatch(pl->rec->storageTests, ds_, zeta ? zeta->format : 0)) {
            *why = "an ordered storage variant whose depth/stencil tests differ from the draw";
            cb_->refusal = {RefDepthStencil, pl->rec->storageTests,
                            uint8_t(fragmentTests(ds_, zeta ? zeta->format : 0)), 0};
            return false;
        }
        // A pass that declares no attachment tests depth and stencil against its hidden one (PassDesc::hiddenZeta), which the
        // pass is given when its recorded states need it; the pipeline was made for the attachment the pass declares
        const Texture *declared = hiddenZeta_ ? nullptr : zeta;
        if (!depthStencilFits(pl->depthFormat, declared ? declared->format : 0, zeta != nullptr, ds_)) {
            *why = "a depth/stencil state or format the pass's depth/stencil attachment does not take";
            cb_->refusal = {RefDepthStencil, pl->depthFormat, declared ? declared->format : 0, ds_.active()};
            return false;
        }
        if (!samplesDrawn(pl->samples, samples_, pl->samplePasses) ||
            !sampleShadingMasks(samples_, pl->samples == samples_ ? pl->samplePasses : 1, compatibleSampleMasks_)) {
            *why = "a state whose samples a pixel or per-sample shading the pass's attachments do not take: skipped";
            cb_->skipped = {RefSamples, pl->samples, samples_, pl->samplePasses};
            cb_->skips++;
            skipDraw_ = true;
            return false;
        }
        compatibleRasterMask_ = sampleMaskFor(pl->samples, samples_);
        if (rot_.turned() && pl->turnRefused) {
            *why =
                "a state drawn into a turned plane whose record holds no turned fragment program (PipelineTurnRefused)";
            cb_->refusal = {RefTurnRefused, uint32_t(pl->rec->keyHash), uint32_t(pl->rec->keyHash >> 32), rot_.turn};
            return false;
        }
        compatible_ = pl;
    }
    if (!prepareShaderTests(*pl, why))
        return false;
    if (p_.room() < 1024 + pl->methods.size() + (cb_->extrasLive_ ? cb_->extrasLive_->restore.size() : 0) +
                        (views_.dirtyViewports | views_.dirtyScissors ? 24 * kMaxViewports : 0))
        reserve();
    Device &d = *dev_;
    Push &p = p_;
    // NVK's dynamic state is dirty at a command buffer's first draw: its primitive restart index (0 then; an index buffer bind
    // has set its own before an indexed one) is written there
    if (!cb_->drawn_) {
        cb_->drawn_ = true;
        const uint32_t restart = indexed ? cb_->restart_ : 0;
        if (!indexed && cb_->restart_ != restart) {
            p.m(DA_PRIMITIVE_RESTART_INDEX, restart);
            cb_->restart_ = restart;
        }
    }
    const bool dsChanged = dsDirty_;
    if (dsDirty_) {
        RAW_ENCODE_SPAN(Bind);
        const bool depth = zeta && zetaHasDepth(zeta->format), stencil = zeta && zetaHasStencil(zeta->format);
        const bool fixedStencil = stencil && !shadowStencil_, shaderTests = shaderTestsActive_ && !shadowStencil_;
        RAW_DIAGNOSTIC(cb_->counts_.segmentCopies += !shaderTests && !measuringInline() && !bindsBySegment() &&
                                                     dsState_->segment(depth, fixedStencil));
        if (shaderTests)
            emitDepthStencil(p, shaderDepthStencilRegs(depth), cb_->ds_);
        else if (bindDepthStencil(p, bodies_, bodyStart_,
                                  dsState_->regs(depth, fixedStencil, stencilRef_[0], stencilRef_[1]), cb_->ds_,
                                  bindsBySegment() ? dsState_->segment(depth, fixedStencil) : StateSegment())) {
            cb_->segmentBinds_++;
            RAW_DIAGNOSTIC(cb_->counts_.segmentBinds++);
        }
        dsDirty_ = false;
    }
    if (zcull_.candidate && (dsChanged || pl != zcullProgram_)) {
        zcullProgram_ = pl;
        if (zcullInvalidates(zcull_, ds_, zeta && zetaHasDepth(zeta->format), zeta && zetaHasStencil(zeta->format),
                             pl->fragmentDepth, shaderTestsActive_ != 0)) {
            p.m(kInvalidateZcull, 0);
            RAW_DIAGNOSTIC(d.stats.zcullInvalidations++);
        }
    }
    uint32_t *root = cb_->rootPush_;
    const PackPipeline &r = *pl->rec;
    // set 0 first (it may refuse the draw before anything is written): the layout's entries only
    if (!pl->ubosNamed) {
        *why = "a program reads a set-0 uniform buffer its record does not name";
        cb_->refusal = {RefUnnamedUniform};
        return false;
    }
    if (__atomic_load_n(&pl->heapMissing, __ATOMIC_ACQUIRE) && !dev_->rebindArguments(*pl)) {
        *why = "no memory for the tables a state reads argument buffers through";
        cb_->refusal = {RefHeapMemory};
        return false;
    }
    const SetEntries &entries = pl->entries;
    if ((spillEntryOf(r, 0) && !pl->spill[0]) || (spillEntryOf(r, 1) && !pl->spill[1])) {
        *why = "an argument table whose words its record does not hold";
        cb_->refusal = {RefSpillTable, spillEntryOf(r, 0) && !pl->spill[0] ? 1u : 2u};
        return false;
    }
    const uint32_t setBytes = renderSetBytes(r.setBytes);
    const auto upload = [&](const uint32_t *words) {
        RAW_ENCODE_COUNT(DescriptorUploads);
        uint64_t address = 0;
        uint8_t *at = d.alloc(d.upload_, setBytes, 256, address);
        memcpy(at, words, setBytes);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        RAW_DIAGNOSTIC(cb_->counts_.uploadBytes += setBytes);
#endif
        return address;
    };
    const uint32_t *set = nullptr;
    const SetResiduals *built = nullptr;
    uint64_t setVa = 0;
    RAW_ENCODE_BEGIN(descriptorSpan, Descriptors);
    if (!entries.wide() && setBytes <= kRenderSetBytes) {
        wideDescriptors_.forget();
        const auto *prepared = descriptors_.resolve(
            pl, movedEntries(entries), pl->memoryEntries,
            [&](PreparedDescriptorWords &prepared, uint32_t moved) {
                RAW_ENCODE_COUNT(DescriptorBuilds);
                uint32_t *set = prepared.words.data();
                for (uint32_t i = 0; i < entries.size(); i++)
                    if ((moved >> i & 1) && !writeSetEntry(pl, entries[i], set, prepared, why))
                        return false;
                return fillSpills(*pl, moved, false, prepared, set, why);
            },
            [&](const DescriptorWords &words) {
                return upload(words.data());
            },
            setBytes);
        if (prepared)
            set = prepared->words.data(), built = prepared, setVa = prepared->address;
    } else {
        descriptors_.leave();
        const auto *prepared = wideDescriptors_.resolve(
            pl, anyEntryMoved(entries), setBytes,
            [&](WideDescriptorWords &prepared) {
                RAW_ENCODE_COUNT(DescriptorBuilds);
                uint32_t *set = prepared.words.data();
                for (uint32_t i = 0; i < entries.size(); i++)
                    if (!writeSetEntry(pl, entries[i], set, prepared, why))
                        return false;
                return fillSpills(*pl, 0, true, prepared, set, why);
            },
            [&](const std::vector<uint32_t> &words) {
                return upload(words.data());
            });
        if (prepared)
            set = prepared->words.data(), built = prepared, setVa = prepared->address;
    }
    RAW_ENCODE_END(descriptorSpan);
    if (!set)
        return false;
    moved_ = MovedBindings{{}, {}, {}, false};
    spillDirty_ = false;
    const auto &residuals = built->residuals;
    const uint32_t residualMask = built->residualMask;
    prepared_ = pl;
    preparedSet_ = setVa;
    RAW_ENCODE_BEGIN(bindSpan, Bind);
    const Raster raster = rasterFor(primitive);
    writeRaster(p, raster);
    const bool dynamicBlend = r.blendColorWord || r.halfBlendColorWord;
    const bool slotRoots = r.rootSlots[0] || r.rootSlots[1];
    const bool patches = tessellates(r);
    const bool drawnWordsDirty = ((r.amplificationIdWord || r.amplificationCountWord || r.amplificationLayerWord ||
                                   r.amplificationViewportWord) &&
                                  amplificationDirty_) ||
                                 r.pulledStride[0][1] || r.arrayLengthWord || r.viewportCountWord;
    const bool rebinds = pl != emitted_;
    if (pl != emitted_ || residualMask || residualsPushed_ || (dynamicBlend && blendColorDirty_) ||
        r.shaderTests.flags || shadowStencil(r.shaderTests) || slotRoots || (patches && tessWordsDirty_) ||
        drawnWordsDirty) {
        if (pl != emitted_) {
            blendColorDirty_ |= pl->constantBlend || dynamicBlend;
            if (cb_->extrasLive_ && cb_->extrasLive_ != pl) {
                p.raw(cb_->extrasLive_->restore.data(), uint32_t(cb_->extrasLive_->restore.size()));
                cb_->extrasLive_ = nullptr;
                cb_->heldSegment_.lose();
            }
            // the iterated blend unit refuses a draw binding more than one colour target (class error 0x59)
            const uint32_t targets = colorTargets();
            const bool narrow = pl->freeForm.rung == FetchIteratedBlend && targets > 1;
            if (narrow != narrowed_) {
                p.m(CT_SELECT, targetSelect(narrow ? 1 : targets));
                narrowed_ = narrow;
            }
            bindState(p, *pl);
            clipHeld_ = pl->viewportClip;
            clampRangeHeld_ = clampRangeHeld_ && !pl->namesViewportClipRange;
            // a vertex program's Layer selects the slice where it writes one (NVK: the last pre-rasterization stage's)
            if (const uint32_t rtLayer = pl->writesLayer ? kRtLayerFromShader : 0; rtLayer != cb_->rtLayer_) {
                p.m(NVB197_SET_RT_LAYER, rtLayer);
                cb_->rtLayer_ = rtLayer;
            }
            RAW_DIAGNOSTIC(cb_->counts_.blendKillBinds += pl->blendKill);
            if (!pl->restore.empty())
                cb_->extrasLive_ = pl;
        }
        // the state's push words into the root table, as m2v's vkCmdPushConstants ranges reach it through NVK: the words its
        // ranges cover (the rest keep what an earlier state wrote), the draw's residuals in theirs, one LOAD_CONSTANT_BUFFER per
        // run of words that change
        uint32_t want[32];
        memcpy(want, pl->push, sizeof want);
        if (patches) {
            controlWords(r, want);
            tessWordsDirty_ = false;
        }
        drawnWords(*pl, want);
        for (uint32_t m = residualMask; m; m &= m - 1)
            want[__builtin_ctz(m)] = residuals[__builtin_ctz(m)];
        if (!blendWords(r, why))
            return false;
        patchBlendPush(r, blendColor_, blendWords_, want);
        patchShaderTestPush(r.shaderTests, ds_, stencilRef_, depthRange_, false, want);
        for (uint32_t k = 0; k < 2; k++)
            if (const uint8_t word = r.rootTableWord[k]; r.rootSlots[k] && word && word <= 31) {
                const uint64_t table = rootTable(r, k, pl->writesMemory[k]);
                want[word - 1] = uint32_t(table);
                want[word] = uint32_t(table >> 32);
            }
        for (int i = 0; i < 32;) {
            if (!(pl->pushMask >> i & 1) || root[i] == want[i]) {
                i++;
                continue;
            }
            int j = i;
            while (j < 32 && (pl->pushMask >> j & 1) && root[j] != want[j])
                j++;
            p.m(LOAD_CB_OFFSET, 0x700 + 4 * i);
            p.nonInc(LOAD_CB, want + i, j - i);
            memcpy(root + i, want + i, (j - i) * 4);
            i = j;
        }
        emitted_ = pl;
        residualsPushed_ = residualMask != 0;
    }
    writeBiasUnits(p, raster);
    writeAlphaControl(p, cb_->alphaHeld_, alphaWanted_, rebinds, pl->namesAlphaControl, pl->ownsAlphaControl,
                      pl->blendKill);
    if (rebinds && cb_->sampleMaskHeld_ != kSampleMaskAll)
        cb_->sampleMaskHeld_ = kSampleMaskUnknown;
    if (compatibleRasterMask_ != cb_->sampleMaskHeld_) {
        const uint32_t m = compatibleRasterMask_;
        p.inc(NVB197_SET_SAMPLE_MASK_X0_Y0, {m, m, m, m});
        cb_->sampleMaskHeld_ = m;
    }
    RAW_ENCODE_END(bindSpan);
    if (!writeClipControl(p, *pl, raster, why))
        return false;
    if (samplePasses_ != pl->samplePasses) {
        p.m(LOAD_CB_OFFSET, kRootSampleInfo + 8);
        p.nonInc(LOAD_CB, compatibleSampleMasks_, 4);
        samplePasses_ = pl->samplePasses;
    }
    const uint32_t topo = nvTopology(primitive), restart = (primitive == 2 || primitive == 4) ? 1 : 0;
    if (topo != topology_) {
        p.m(PRIMITIVE_TOPOLOGY, topo);
        topology_ = topo;
    }
    if (restart != restart_) {
        p.m(DA_PRIMITIVE_RESTART, restart);
        restart_ = restart;
    }
    if (viewportDirty_) {
        switch_gm20b_viewport_state v = viewportState(viewport_);
        turnViewport(rot_, v);
        kit(p, switch_gm20b_viewport, 0u, &v);
        viewportDirty_ = false;
    }
    if (scissorDirty_) {
        const Rect s = planeRect(scissor_[0] & 0xffff, scissor_[1] & 0xffff, scissor_[0] >> 16, scissor_[1] >> 16);
        kit(p, switch_gm20b_scissor, 0u, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
        scissorDirty_ = false;
    }
    if (views_.dirtyViewports | views_.dirtyScissors)
        emitViewports(p);
    // NVK's SET_BLEND_CONST_RED..ALPHA, the floats' bits, written where a draw blends with them
    if ((pl->constantBlend || dynamicBlend) && blendColorDirty_) {
        p.incArray(BLEND_CONST_RED, blendColor_, 4);
        blendColorDirty_ = false;
    }
    bindStreams(p, *pl, indexed, vFirst, count);
    bool selected = false;
    if (descriptorBinding_.changed(pl, setVa)) {
        const HeapSet *heap = pl->heapSet;
        const uint64_t heapVa = heap ? heap->memory.va : 0;
        // NVK's root table sets[0] and sets[1]: set 0, and set 1 for a heap array a program reads past its constant buffer
        const uint32_t s[8] = {uint32_t(setVa),        uint32_t(setVa >> 32),  setBytes, 0, uint32_t(heapVa),
                               uint32_t(heapVa >> 32), heap ? r.heapBytes : 0, 0};
        p.m(LOAD_CB_OFFSET, 0x38);
        p.nonInc(LOAD_CB, s, heap ? 8 : 4);
        if (pl->heapSlot[0] || pl->heapSlot[1] || pl->heapSlot[2] || pl->heapSlot[3]) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(r.heapBytes), uint32_t(heapVa >> 32), uint32_t(heapVa)});
            for (uint32_t k = 0; k < 4; k++)
                if (pl->heapSlot[k])
                    p.m(BIND_GROUP_CB + 0x20 * kSetGroups[k], 1 | uint32_t(pl->heapSlot[k]) << 4);
            selected = true;
        }
        const bool setRead = pl->setSlot[0] || pl->setSlot[1] || pl->setSlot[2] || pl->setSlot[3];
        if (setRead) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(setBytes), uint32_t(setVa >> 32), uint32_t(setVa)});
            for (uint32_t k = 0; k < 4; k++)
                if (pl->setSlot[k])
                    p.m(BIND_GROUP_CB + 0x20 * kSetGroups[k], 1 | uint32_t(pl->setSlot[k]) << 4);
            selected = true;
        }
    }
    // NVK_MME_BIND_CBUF_DESC's bindings, read from cached CPU words rather than the WC upload.
    for (uint8_t u = 0; u < pl->uboCount; u++) {
        const Pipeline::UboCbuf &c = pl->ubos[u];
        const uint32_t *desc = set + entries[c.entry].offset / 4;
        if (!descriptorBinding_.uboMoved(u, desc))
            continue;
        if (desc[2]) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(desc[2]), desc[1], desc[0]});
            p.m(BIND_GROUP_CB + 0x20 * c.group, 1 | uint32_t(c.slot) << 4);
            selected = true;
        } else {
            p.m(BIND_GROUP_CB + 0x20 * c.group, uint32_t(c.slot) << 4);
        }
    }
    if (selected)
        p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(d.root_.va >> 32), uint32_t(d.root_.va)});
    if (pl->splitOf)
        launchSplit(p, *pl, set);
    else if (launchOnly)
        launchSplit(p, *launchOnly, set);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb_->splitCapturing_ && twin->splitOf && !indexed)
        captureSplit(*pl, geometryAnalysis, primitive, vFirst, count);
#endif
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    countRules(*pl);
#endif
    // framebuffer fetch reads a color attachment through the texture unit: a barrier where a draw since the last one wrote
    // any color, as m2v's barrier (0106) follows any color write, whichever colors this draw fetches.
    // The barrier holds this draw's pixel shading until the earlier pixels have left the ROP; vertex work and rasterization
    // go on (NVK's for a color-output to fragment-read dependency, as NVIDIA's drivers order one)
    // A storage access in the interlock is ordered after the earlier ones by the ticket, not after the ROP's writes; the
    // ROP's accesses are ordered after neither: a barrier where a texture fetch meets a write of either kind, a storage
    // access meets a ROP write, or a ROP access (a write, or the read a blend or a partial write mask makes) meets a
    // storage write.
    // A fetch through the texture unit that meets only ROP writes needs the barrier only where its pixels meet theirs
    // (raw_region.h); nothing bounds a storage access's pixels.
    RAW_ENCODE_BEGIN(barrierSpan, Barriers);
    const AttachmentAccess access{pl->readBits, pl->writeBits, uint8_t(pl->fetchBits & ~pl->storageBits),
                                  pl->storageBits};
    if (firstDraw && !indirectGeometry_ && !pl->dropsPixels && !(alphaWanted_ & kAlphaToCoverage)) {
        const uint8_t opaque = pl->writeBits & ~(pl->readBits | pl->fetchBits | pl->storageBits);
        if (opaque && geometryAnalysis.covers(primitive == 3, areaW_, areaH_))
            covered_ = opaque;
    }
    const AttachmentBarrier needed = access.after(attachmentWrites_.rop, attachmentWrites_.storage);
    if (needed != AttachmentBarrier::None) {
        if (needed == AttachmentBarrier::Pixel && !written_.meets(geometryAnalysis)) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            RAW_DIAGNOSTIC(dev_->stats.fetchBarriersElided++);
#endif
#ifdef SWITCH_RAW_HARNESS_MARKS
            // the host frame harness's builds only (test/build.sh): nvdec names this mark ELIDED, and eqcheck takes a
            // barrier NVK has and raw left out only where it is
            p.m(NO_OPERATION, 0x0e11de);
#endif
        } else {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            RAW_DIAGNOSTIC(dev_->stats.fetchBarriers++);
#endif
            if (needed == AttachmentBarrier::Shader) {
                RAW_DIAGNOSTIC(d.stats.storageBarriers++);
                tiledCacheDrain(p, d.k_.info());
                p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
                p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
            } else {
                fetchBarrier(p);
            }
            attachmentWrites_.synchronized(needed);
            written_.clear();
        }
    }
    RAW_ENCODE_END(barrierSpan);
#if SWITCH_RAW_DIAGNOSTICS
    if (pipeline_ && pipeline_->caName)
        probeDraw(*pipeline_, *pl, primitive, first, count, indexed, {x[0], y[0], x[1], y[1]});
#endif
    RAW_DIAGNOSTIC(cb_->counts_.interlockDraws += pl->storageBits != 0);
    RAW_DIAGNOSTIC(cb_->counts_.rasterOrderedDraws += pl->rec && (pl->rec->flags & PipelineRasterOrdered));
#if SWITCH_RAW_DIAGNOSTICS
    if (pl->rec && pl->storageBits)
        dev_->stats.fetch.interlockKeys.note(pl->rec->keyHash);
    if (pl->rec && pl->genericState)
        dev_->stats.fetch.genericKeys.note(pl->rec->keyHash);
#endif
    RAW_DIAGNOSTIC(cb_->counts_.interlocked += (pl->fetchBits & pl->storageBits) != 0);
    RAW_DIAGNOSTIC(cb_->counts_.interlockedBlends += pl->storageBlendBits != 0);
    attachmentWrites_.rop |= pl->writeBits;
    attachmentWrites_.storage |= pl->storageWriteBits;
    if (pl->writeBits)
        written_.add(geometryAnalysis);
    // Reuse a bound already established for split/coverage/fetch decisions. Admission never triggers new CPU geometry
    // reads; unknown geometry uses the clipped viewport/scissor extent rather than the whole render target.
    drawBox_ = geometryAnalysis.cachedBox();
    drawPixels_ = drawBox_.empty() ? 0 : uint64_t(drawBox_.x1 - drawBox_.x0) * (drawBox_.y1 - drawBox_.y0);
    return pl->freeForm.matrixSlot == kNoUniform || matrixConstants(*pl, why);
}

// A colour matrix form's draw: its iterated blend program and constants from the matrix uniform the draw binds, which
// the GPU's macro reads through a non-prefetched GPFIFO entry over the uniform's bytes (the CPU reads none of them). A
// uniform of the rows alone (a QuartzCore whose matrix has no clamp) is followed by a clamp pair of 0 in an entry of its
// own: the macro's method data runs on across the entries, each but the last incomplete, which the kernel takes only
// before a non-prefetched entry.
bool RenderEncoder::matrixConstants(const Pipeline &pl, const char **why)
{
    static_assert(kMatrixWords == SWITCH_GM20B_MACRO_VCM_ARGUMENTS, "the colour matrix macro's arguments");
    const Bound &b = bufs_[2][pl.freeForm.matrixSlot];
    const uint32_t words = pl.freeForm.matrixWords;
    if (!b.va || uint64_t(pl.freeForm.matrixOffset) + 4 * words > b.size || ((b.va + pl.freeForm.matrixOffset) & 3)) {
        *why = "a colour matrix form's draw binds no uniform buffer that holds the matrix";
        cb_->refusal = {};
        return false;
    }
    if (b.buffer && b.buffer->hostVisible && !b.buffer->wc && b.cpu)
        cb_->clean(b.cpu + pl.freeForm.matrixOffset, 4 * words);
    mme::matrixHeader(p_);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    const uint32_t clampWords = kMatrixWords - words;
    uint64_t clampVa = 0;
    if (clampWords) {
        auto *clamp = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 4 * clampWords, 4, clampVa));
        std::fill(clamp, clamp + clampWords, 0u);
    }
    matrixCall(bodies_, bodyStart_, p_.at, b.va + pl.freeForm.matrixOffset, words, clampVa, clampWords);
    bodyStart_ = nullptr;
    reserve();
    return true;
}

// What the region test reads of a draw: Core Animation's transform (Pipeline::caTransform) over a triangle list or strip
// of one instance, with its matrix, positions and indices where the CPU maps them; anything else is its scissor alone
bool RenderEncoder::entryMoved(const PackEntry &e) const
{
    const bool buffer = e.kind == EntryUbo && (moved_.buffers[e.stage] >> e.slot & 1);
    const bool texture = (e.kind == EntryTexture || e.kind == EntryTexture3d) && e.slot < kStageTextures &&
                         hasSlot(moved_.textures[handleStage(e)], e.slot);
    const bool sampler =
        e.kind != EntryUbo && e.samplerKind == SamplerSlot && (moved_.samplers[handleStage(e)] >> e.sampler & 1);
    const bool spill = e.kind == EntrySpillTable && (moved_.buffers[e.stage] || (e.stage == 2 && spillDirty_));
    return buffer || texture || sampler || spill;
}

// The entries of a set of kPackEntries or fewer whose bindings moved since the last draw, bit k entry k
uint32_t RenderEncoder::movedEntries(const SetEntries &entries) const
{
    if (moved_.all)
        return ~0u;
    uint32_t moved = 0;
    for (uint32_t i = 0; i < entries.size(); i++)
        moved |= uint32_t(entryMoved(entries[i])) << i;
    return moved;
}

bool RenderEncoder::anyEntryMoved(const SetEntries &entries) const
{
    if (moved_.all)
        return true;
    for (uint32_t i = 0; i < entries.size(); i++)
        if (entryMoved(entries[i]))
            return true;
    return false;
}

// The extended blend colour's words (raw_blend_color.h) where `r` reads them, made again after the colour moved
bool RenderEncoder::blendWords(const PackPipeline &r, [[maybe_unused]] const char **why)
{
    if (!r.blendColorWord || !blendWordsDirty_)
        return true;
    float color[4];
    memcpy(color, blendColor_, sizeof color);
    blendColorWords(color, blendWords_);
    blendWordsDirty_ = false;
    return true;
}

// `pl`'s argument tables (raw_spill_table.h) whose entries moved: the record's words with the spilled bind offsets'
// residuals and a fragment table's dynamic words, uploaded where they differ from the encoder's last upload, named in set 0
bool RenderEncoder::fillSpills(const Pipeline &pl, uint32_t moved, bool all, SetResiduals &prepared, uint32_t *set,
                               const char **why)
{
    const PackPipeline &r = *pl.rec;
    Device &d = *dev_;
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t entry = spillEntryOf(r, k), count = r.spillWords[k];
        if (!entry || !(all || (entry <= 32 && moved >> (entry - 1) & 1)))
            continue;
        uint32_t words[kSpillWordsMax];
        composeSpill(pl.spill[k], count, prepared.spilled[k], words);
        if (k == 1) {
            if (!blendWords(r, why))
                return false;
            patchBlendPush(r, blendColor_, blendWords_, words, kRootPushWords, count);
            patchShaderTestPush(r.shaderTests, ds_, stencilRef_, depthRange_, false, words, kRootPushWords, count);
        }
        const uint64_t va = spillUploads_[k].resolve(words, count, [&](const uint32_t *from, uint32_t n) {
            RAW_ENCODE_COUNT(DescriptorUploads);
            uint64_t address = 0;
            uint8_t *upload = d.alloc(d.upload_, spillRange(n), 256, address);
            memcpy(upload, from, n * sizeof(uint32_t));
            return address;
        });
        spillDescriptor(va, count, set + pl.entries[entry - 1].offset / 4);
    }
    return true;
}

DrawGeometry RenderEncoder::geometry(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count,
                                     const PixelBox &limit)
{
    DrawGeometry g;
    g.limit = limit;
    g.viewport = {vp_[0], vp_[1], vp_[2], vp_[3]};
    g.first = first;
    g.count = count;
    g.indices = drawIndices_;
    g.indexSize = drawIndices_ ? drawIndexSize_ : 0;
    g.strip = primitive == 4;
    const Bound &m = bufs_[1][2];
    const Bound &v = pl.rec->vertexSlot < 31 ? bufs_[1][pl.rec->vertexSlot] : bufs_[1][0];
    g.bounded = !indirectGeometry_ && pl.caTransform && pl.positionComponents && (primitive == 3 || primitive == 4) &&
                m.cpu && m.size >= 64 && v.cpu && pl.rec->vertexSlot < 31;
    g.bounded &= !g.indices || drawIndexCpuSafe_ || drawIndexBuffer_;
    g.owner = cb_;
    g.inputs = {m.buffer, v.buffer, g.indices && !drawIndexCpuSafe_ ? drawIndexBuffer_ : nullptr};
    g.acquire = [](void *owner, const void *input, const uint8_t *cpu, uint64_t bytes) {
        return static_cast<CommandBuffer *>(owner)->acquireGeometry(*static_cast<const Buffer *>(input), cpu, bytes);
    };
    if (g.bounded) {
        g.matrix = m.cpu;
        g.positions = {v.cpu, pl.rec->vertexStride, pl.positionOffset, pl.positionComponents};
        g.positionBytes = v.size;
    }
    return g;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
const char *Device::fetchBarrierName(FetchBarrier b)
{
    switch (b) {
    case FetchBarrier::PixelShader:
        return "ps";
    case FetchBarrier::WaitForIdle:
        return "wfi";
    case FetchBarrier::PixelShaderOnly:
        return "psonly";
    case FetchBarrier::None:
        return "none";
    }
    return "?";
}
#endif

void RenderEncoder::fetchBarrier(Push &p)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    switch (dev_->fetchBarrier.load(std::memory_order_relaxed)) {
    case Device::FetchBarrier::None:
        return;
    case Device::FetchBarrier::WaitForIdle:
        p.m(WAIT_FOR_IDLE, 0);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
        return;
    case Device::FetchBarrier::PixelShaderOnly:
        p.m(PIXEL_SHADER_BARRIER, 0);
        return;
    case Device::FetchBarrier::PixelShader:
        break;
    }
#endif
    kit(p, switch_gm20b_pixel_shader_barrier);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void RenderEncoder::countRules(const Pipeline &pl)
{
    if (pl.fetchBits && anyWritten_) {
        RAW_DIAGNOSTIC(dev_->stats.fetchBarriersAnyColor++);
        anyWritten_ = false;
    }
    anyWritten_ |= pl.writeBits != 0;
    const uint8_t stored = pl.fetchBits & pl.writeBits, ropWrites = pl.writeBits & ~pl.fetchBits;
    if ((pl.fetchBits & ropWritten_) || (ropWrites & storageWritten_)) {
        RAW_DIAGNOSTIC(dev_->stats.fetchBarriersInterlocked++);
        ropWritten_ = storageWritten_ = 0;
    }
    ropWritten_ |= ropWrites;
    storageWritten_ |= stored;
}

#endif

bool RenderEncoder::prepareShaderTests(const Pipeline &pipeline, const char **why)
{
    const PackShaderTests &tests = pipeline.rec->shaderTests;
    const uint8_t flags = tests.flags;
    const bool shadow = shadowStencil(tests) && flags == kShaderTestStencil;
    if (shadow != shadowStencil_) {
        shadowStencil_ = shadow;
        dsDirty_ = true;
    }
    if (!flags) {
        endShaderTests();
        return true;
    }
    const Texture *zeta = pass_.depthStencil.texture;
    ShaderTestShape copy;
    if (!zeta || !shaderTestShape(zeta->format, samples_, planeArea().x1, planeArea().y1, flags, copy)) {
        *why = "a shader depth/stencil test whose attachment cannot be copied exactly";
        cb_->refusal = {RefDepthStencil, zeta ? zeta->format : 0, flags, samples_};
        return false;
    }
    if (shadow && shadowDependsOnDepth(ds_, zetaHasDepth(zeta->format))) {
        *why = "a shader-side stencil whose stencil write depends on the depth test after its program";
        cb_->refusal = {RefDepthStencil, zeta->format, flags, ds_.depthCompare};
        return false;
    }
    // a shader-side stencil's depth test is the hardware's, against the attachment
    if (shadow && (shaderTestsActive_ & kShaderTestDepth)) {
        copyShaderTests(kShaderTestDepth, true);
        shaderTestsActive_ &= uint8_t(~kShaderTestDepth);
        dsDirty_ = true;
    }
    const uint8_t missing = flags & ~shaderTestsActive_;
    if (!missing)
        return true;
    auto obtain = [&](Texture *&slot, uint32_t format, uint32_t tag) {
        if (slot)
            return true;
        Texture shape;
        shape.width = zeta->width;
        shape.height = zeta->height;
        shape.format = format;
        shape.samples = samples_;
        shape.layers = std::max(pass_.layers, 1u);
        if (!(slot = dev_->lastBacking(shape, tag, true, false))) {
            RAW_ENCODE_LOCK(lock, dev_->lock_);
            slot = dev_->backing(shape, tag, true, false);
        }
        if (!slot)
            return false;
        cb_->backings_.push_back(slot);
        cb_->use(*slot, true);
        invalidateDescriptors();
        return true;
    };
    constexpr uint32_t kR32Float = 55, kR8Uint = 13, kRG8Unorm = 30;
    if (((missing & kShaderTestDepth) && !obtain(shaderDepth_, kR32Float, Device::kShaderDepthTag)) ||
        ((missing & kShaderTestStencil) && !obtain(shaderStencil_, kR8Uint, Device::kShaderStencilTag)) ||
        ((missing & kShaderTestStencil) && copy.packedStencil &&
         !obtain(shaderStencilTemp_, kRG8Unorm, Device::kShaderStencilTempTag))) {
        *why = "no native backing for shader depth/stencil tests";
        cb_->refusal = {RefDepthStencil, zeta->format, flags, samples_};
        return false;
    }
    copyShaderTests(missing, false);
    shaderTestsActive_ |= missing;
    dsDirty_ = true;
    return true;
}

void RenderEncoder::copyShaderTests(uint8_t flags, bool toAttachment)
{
    if (p_.room() < 256)
        reserve();
    const Texture &zeta = *pass_.depthStencil.texture;
    ShaderTestShape shape;
    shaderTestShape(zeta.format, samples_, planeArea().x1, planeArea().y1, flags, shape);
    auto side = [](const Texture &texture, uint32_t layer) {
        auto result = sideOf(texture, 0, 0, layer);
        result.height = sampleRows(texture);
        return result;
    };
    cb_->use(zeta, toAttachment);
    tiledCacheDrain(p_, dev_->k_.info());
    p_.m(WAIT_FOR_IDLE, 0);
    if (toAttachment)
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
    const auto copies = shaderTestTransfers(flags, shape.packedStencil, toAttachment);
    // every layer of a layered pass: a slice-choosing state's tests read the stand-ins' layers as a 2D array
    for (uint32_t layer = 0; layer < std::max(pass_.layers, 1u); ++layer) {
        const auto image = [&](ShaderTestImage which) {
            switch (which) {
            case ShaderTestImage::depth:
                return side(*shaderDepth_, layer);
            case ShaderTestImage::stencil:
                return side(*shaderStencil_, layer);
            case ShaderTestImage::temporary:
                return side(*shaderStencilTemp_, layer);
            case ShaderTestImage::attachment:
                break;
            }
            return side(zeta, layer);
        };
        for (uint8_t at = 0; at < copies.count; ++at) {
            const auto &copy = copies.transfers[at];
            copyRemapped(p_, image(copy.source), image(copy.destination), shape.width, shape.height, copy.remap,
                         copy.bytes, dev_->stats, copy.ordered);
        }
    }
    RAW_DIAGNOSTIC(dev_->stats.compZetaCopies += zeta.comptags);
    p_.inc(CE_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, (kLaunchPitch & ~3u) | 2u | (1u << 10), SUBC_COPY);
    p_.m(INVALIDATE_SHADER_CACHES, kInvalidateData);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
}

void RenderEncoder::endShaderTests()
{
    if (!shaderTestsActive_)
        return;
    copyShaderTests(shaderTestsActive_, true);
    shaderTestsActive_ = 0;
    dsDirty_ = true;
}

uint32_t RenderEncoder::passViewTic(const Texture &t, uint32_t word0)
{
    AttachmentView v;
    v.word0 = word0;
    for (uint8_t i = 0; i < passViewCount_; ++i)
        if (passViews_[i].ownTic == t.tic && passViews_[i].view == v)
            return passViews_[i].tic;
    const uint32_t tic = dev_->viewTic(t, word0, ViewAccess::Storage);
    if (tic && passViewCount_ < passViews_.size())
        passViews_[passViewCount_++] = {t.tic, v, tic};
    return tic;
}

uint32_t RenderEncoder::passViewTic(const Texture &owner, const AttachmentView &v, const char **why)
{
    for (uint8_t i = 0; i < passViewCount_; ++i)
        if (passViews_[i].ownTic == owner.tic && passViews_[i].view == v)
            return passViews_[i].tic;
    const uint32_t tic = dev_->attachmentViewTic(owner, v, why);
    if (tic && passViewCount_ < passViews_.size())
        passViews_[passViewCount_++] = {owner.tic, v, tic};
    return tic;
}

uint32_t RenderEncoder::fetchTic(const ColorAttachment &a, uint32_t storage, bool layeredRecord, const char **why)
{
    const Texture &t = *a.texture;
    const bool layered = pass_.layers > 1;
    if (t.tic && (storage ? !layeredRecord : !layered))
        return storage ? passViewTic(t, storage) : t.tic;
    const FormatInfo *f = formatInfo(t.format);
    if (!f) {
        *why = "framebuffer fetch from a target the raw path cannot sample";
        return 0;
    }
    AttachmentView v;
    v.word0 = storage ? storage : f->tic0;
    v.srgb = !storage && f->srgb;
    v.arrayed = storage ? layeredRecord : layered;
    v.layers = v.arrayed ? pass_.layers : 1;
    v.part = a.image != nullptr;
    v.level = uint8_t(a.level);
    v.slice = a.slice;
    return passViewTic(v.part ? *a.image : t, v, why);
}

// The rasterizer state of a draw of `primitive` (MTLPrimitiveType, or icb::kPatchPrimitive): the encoder's, with
// Metal's depth bias for its topology: on, of Metal's factors, for triangles and the triangles of patches (an M4 Max
// biases a tessellated quad patch as a triangle); off and zero for points and lines, and zero where both factors are
RenderEncoder::Raster RenderEncoder::rasterFor(uint32_t primitive) const
{
    Raster r = raster_;
    r.bias = icb::triangles(primitive);
    if (r.bias && (depthBias_.constant != 0 || depthBias_.slope != 0)) {
        // Metal's unit of an n-bit UNORM depth is one code, 1 / (2^n - 1) (an M4 Max's Depth16Unorm, macOS 27.2.1)
        const uint32_t bits = depthBias_.unorm24 ? 0 : pass_.biasDepthBits;
        r.units = bits || depthBias_.unorm24 ? Raster::Units::Unorm24 : Raster::Units::Format;
        const double scale = bits ? 16777216.0 / double((1u << bits) - 1) : 1.0;
        r.biasConstant = fbits(float(double(depthBias_.constant) * scale));
        r.biasSlope = fbits(depthBias_.slope);
        r.biasClamp = fbits(depthBias_.clamp);
    }
    return r;
}

}  // namespace raw
