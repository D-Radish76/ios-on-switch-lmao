// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_fragment_tests.h"
#include "raw_render_variant.h"
#include "raw_shader_test_push.h"

static void cubicBinds(__unsafe_unretained RMRenderEncoder *encoder, const raw::Pipeline &pl, raw::CubicBinds &b)
{
    b.samplers.clear();
    b.textures.clear();
    encoder->_cubicTextures[0] = {};
    encoder->_cubicTextures[1] = {};
    encoder->_formSamplers[0] = encoder->_formSamplers[1] = 0;
    for (uint32_t i = 0; i < pl.entries.size(); i++) {
        const raw::PackEntry &e = pl.entries[i];
        if ((e.kind != raw::EntryTexture && e.kind != raw::EntrySamplerOnly) || e.samplerKind != raw::SamplerSlot ||
            (e.stage != 1 && e.stage != 2) || e.sampler >= 16)
            continue;
        encoder->_formSamplers[e.stage - 1] |= uint16_t(1u << e.sampler);
        __unsafe_unretained RMSampler *s = encoder->_samp[e.stage - 1][e.sampler];
        if (!s || !s->_raw.desc.cubic)
            continue;
        const raw::SamplerDesc &d = s->_raw.desc;
        if (std::none_of(b.samplers.begin(), b.samplers.end(), [&](const auto &c) {
                return c.slot == e.sampler && c.stage == e.stage;
            }))
            b.samplers.push_back({e.sampler, d.sAddress, d.tAddress, d.rAddress,
                                  uint8_t(s->_descriptor.borderColor & 3), !d.pixel, e.stage});
        if (e.kind != raw::EntryTexture || e.slot >= raw::kStageTextures)
            continue;
        raw::addSlot(encoder->_cubicTextures[e.stage - 1], e.slot);
        __unsafe_unretained RMTexture *t = encoder->_tex[e.stage - 1][e.slot];
        if (t && std::none_of(b.textures.begin(), b.textures.end(), [&](const auto &c) {
                return c.slot == e.slot && c.stage == e.stage;
            }))
            b.textures.push_back({e.slot, uint32_t(t->_descriptor.pixelFormat), e.stage});
    }
    std::sort(b.samplers.begin(), b.samplers.end(), [](const auto &x, const auto &y) {
        return x.stage != y.stage ? x.stage > y.stage : x.slot < y.slot;
    });
    std::sort(b.textures.begin(), b.textures.end(), [](const auto &x, const auto &y) {
        return x.stage != y.stage ? x.stage > y.stage : x.slot < y.slot;
    });
}

void packedVariants(RMPipeline *p, uint64_t keyHash)
{
    std::lock_guard<raw::Mutex> lock(p->_variantLock);
    for (const auto &[name, pl] : gRM->raw->variantPipelines(keyHash, p->_caTransform)) {
        auto variant = std::make_shared<raw::Variant<raw::Pipeline>>();
        variant->publish(pl);
        p->_variants.emplace(name, std::move(variant));
    }
}

// A variant the compiler refused (`failure` empty) or no compiler made: the command buffers that draw it fail, and its
// key is named once. A shadowed variant's is named by its pass (RawMetalXrShadow.mm reportTwin).
static void settleUnmade(raw::Variant<raw::Pipeline> &variant, const std::string &key, bool shadowed,
                         const std::string &failure)
{
    if (!shadowed) {
        SWITCH_REFUSE_DETAIL("raw", "state.variant", raw::Pack::fnv(key.data(), key.size()),
                             "the shader compiler %s the variant %s%s%s: the command buffers that draw it fail",
                             failure.empty() ? "refused" : "made none of", key.c_str(), failure.empty() ? "" : ": ",
                             failure.c_str());
    }
    if (failure.empty())
        variant.publish(nullptr);
    else
        variant.fail();
}

// Binding and pass preparation starts asynchronous variant work. Sampled variants can use stand-ins while
// pending; other variants hold the queue head until they publish or fail (RawMetalSchedule.mm variantReadiness).
// A compiler's answer about a key holds for the process: refused or not made, the variant stays so, and keeps the
// object plans name. An ask that found no compiler to ask is no answer: the next pass that needs the variant asks
// again, and that ask checks the compiler as a state's creation does (RawComplete.mm Completion::request).
static raw::Variant<raw::Pipeline> *renderVariant(RMPipeline *p, const raw::RenderVariant &forms)
{
    const std::string name = forms.name();
    std::shared_ptr<raw::Variant<raw::Pipeline>> variant;
    {
        std::lock_guard<raw::Mutex> lock(p->_variantLock);
        auto [at, inserted] = p->_variants.try_emplace(name);
        if (inserted)
            at->second = std::make_shared<raw::Variant<raw::Pipeline>>();
        else if (!at->second->reopen())
            return at->second.get();
        variant = at->second;
    }
    const std::string key = p->_key + name;
    const bool shadowed = forms.xrShadows != 0;
    raw::Completion *completion = gRM->completion.get();
    if (!completion) {
        variant->failUnserved();
        return variant.get();
    }
    raw::Completion::Ask request;
    request.descriptor = p->_madeFrom;
    request.facade = p->_madeFrom;
    request.key = key;
    request.caTransform = p->_caTransform;
    request.own = !p->_packable;
    request.variant = forms;
    request.resolveDescriptor = true;
    completion->askVariant(std::move(request),
                           [variant, key, shadowed](raw::Pipeline *made, const std::string &failure) {
                               if (made)
                                   variant->publish(made);
                               else if (failure == raw::Completion::kNoCompilerComes)
                                   variant->failUnserved();
                               else
                                   settleUnmade(*variant, key, shadowed, failure);
                           });
    return variant.get();
}

static void awaitSettled(const std::vector<raw::Variant<raw::Pipeline> *> &variants)
{
    if (variants.empty())
        return;
    dispatch_semaphore_t settled = dispatch_semaphore_create(0);
    size_t pending = 0;
    for (raw::Variant<raw::Pipeline> *variant : variants) {
        const auto state = variant->observe([settled] {
            dispatch_semaphore_signal(settled);
        });
        if (state == raw::Variant<raw::Pipeline>::State::Pending)
            pending++;
    }
    for (; pending; pending--)
        dispatch_semaphore_wait(settled, DISPATCH_TIME_FOREVER);
}

// The test classes drawnTests (raw_shader_test_push.h) can give a draw of `pl` in a pass of the creation's depth and
// stencil formats, whatever its depth/stencil state
static std::vector<uint8_t> testClasses(const raw::Pipeline &pl, uint32_t depth, uint32_t stencil)
{
    using raw::FragmentTests;
    const bool hasDepth = raw::zetaHasDepth(depth) || raw::zetaHasDepth(stencil);
    const bool hasStencil = raw::zetaHasStencil(depth) || raw::zetaHasStencil(stencil);
    if (pl.fetchBits && pl.depthFormat && !pl.storageBits) {
        std::vector<uint8_t> classes = {uint8_t(FragmentTests::Pass), uint8_t(FragmentTests::ReadOnly)};
        if (hasDepth)
            classes.push_back(uint8_t(FragmentTests::DepthWrite));
        if (hasStencil)
            classes.push_back(uint8_t(FragmentTests::StencilWrite));
        return classes;
    }
    if (raw::shadowStencil(pl.rec->shaderTests) && hasDepth)
        return {uint8_t(FragmentTests::StencilWrite)};
    return {};
}

// Every test class a draw can take, and its two-plane form with theirs: a packed state's are the pack's
// (directpack.mm makeState, which leaves out a shader-side stencil's). Asked together and waited for as the state's
// own record was, so no draw of them waits on a compile at its queue's head.
void rmMadeVariants(RMPipeline *p, MTLRenderPipelineDescriptor *d)
{
    const std::vector<uint8_t> classes =
        testClasses(*p->_raw, uint32_t(d.depthAttachmentPixelFormat), uint32_t(d.stencilAttachmentPixelFormat));
    const bool twoPlane =
        std::any_of(std::begin(p->_raw->colorFormats), std::end(p->_raw->colorFormats), [](uint32_t format) {
            return raw::xrForm(format) == raw::XrForm::Planes;
        });
    std::vector<raw::Variant<raw::Pipeline> *> made;
    for (const bool planes : {false, true}) {
        if (planes && !twoPlane)
            break;
        raw::RenderVariant forms;
        forms.twoPlane = planes;
        if (planes)
            made.push_back(renderVariant(p, forms));
        for (const uint8_t tests : classes) {
            forms.fragmentTests = tests;
            made.push_back(renderVariant(p, forms));
        }
    }
    awaitSettled(made);
}

// The textures a draw of `pl` writes in other formats than its record's programs store (raw_written_textures.h)
static void reboundTextures(const raw::Pipeline &pl, RMTexture *const textures[raw::kStageTextures],
                            std::vector<raw::WrittenTexture> &out)
{
    out.clear();
    for (uint32_t slots = pl.writtenSlots; slots; slots &= slots - 1) {
        const uint32_t slot = uint32_t(__builtin_ctz(slots));
        const uint32_t format = textures[slot] ? uint32_t(textures[slot]->_descriptor.pixelFormat) : 0u;
        if (format && format != pl.rec->writtenFormat[slot])
            out.push_back({uint8_t(slot), format});
    }
}

// The slots of each stage (vertex, fragment: raw::handleStage) a draw of `pl` samples a two-plane RGB10A8_2P_XR10
// texture at (raw_xr_pass.h), or (`srgb`) a two-plane RGB10A8_2P_XR10_sRGB one, which no variant reads in its planes: its
// sRGB codes are read through its stand-in alone (raw_xr.h xrConversionOf)
static std::array<raw::StageTextureMask, 2> xrSampledSlots(const raw::Pipeline &pl,
                                                           RMTexture *const textures[2][raw::kStageTextures],
                                                           bool srgb = false)
{
    std::array<raw::StageTextureMask, 2> slots{};
    for (uint32_t i = 0; i < pl.entries.size(); i++) {
        const raw::PackEntry &e = pl.entries[i];
        __unsafe_unretained RMTexture *t =
            e.kind == raw::EntryTexture && e.slot < raw::kStageTextures ? textures[raw::handleStage(e)][e.slot] : nil;
        if (t && t->_raw.xrPlanes && !t->_raw.memoryless && (t->_raw.format == raw::kRgb10A8XrSrgb) == srgb)
            raw::addSlot(slots[raw::handleStage(e)], e.slot);
    }
    return slots;
}

raw::Variant<raw::Pipeline> *rmRenderVariant(RMPipeline *p, const raw::CubicBinds &binds, uint8_t tests, bool planes,
                                             uint16_t xrSlots, const std::vector<raw::WrittenTexture> &written,
                                             uint8_t xrShadows)
{
    return renderVariant(p, {binds, tests, planes, xrSlots, written, xrShadows});
}

static bool sameForms(const raw::RenderVariant &a, const raw::RenderVariant &b)
{
    const auto sameSampler = [](const raw::CubicBinds::Sampler &x, const raw::CubicBinds::Sampler &y) {
        return x.slot == y.slot && x.s == y.s && x.t == y.t && x.r == y.r && x.border == y.border &&
               x.normalized == y.normalized && x.stage == y.stage;
    };
    const auto sameTexture = [](const raw::CubicBinds::Texture &x, const raw::CubicBinds::Texture &y) {
        return x.slot == y.slot && x.format == y.format && x.stage == y.stage;
    };
    return a.fragmentTests == b.fragmentTests && a.twoPlane == b.twoPlane && a.xrSlots == b.xrSlots &&
           a.xrShadows == b.xrShadows && a.written == b.written &&
           std::equal(a.cubic.samplers.begin(), a.cubic.samplers.end(), b.cubic.samplers.begin(),
                      b.cubic.samplers.end(), sameSampler) &&
           std::equal(a.cubic.textures.begin(), a.cubic.textures.end(), b.cubic.textures.begin(),
                      b.cubic.textures.end(), sameTexture);
}

// The state's variant for `forms` as this encoder found it before, else from the state's map. The encoder's command
// buffer retains each state it set, and a state's map never drops a variant, so what it found holds for its pass.
static raw::Variant<raw::Pipeline> *preparedVariant(__unsafe_unretained RMRenderEncoder *e,
                                                    const raw::RenderVariant &forms)
{
    for (const RMPreparedVariant &prepared : e->_prepared)
        if (prepared.pipe == e->_pipe && sameForms(prepared.forms, forms))
            return prepared.variant;
    RMPreparedVariant &slot = e->_prepared[e->_preparedNext++ % e->_prepared.size()];
    slot.pipe = e->_pipe;
    slot.forms = forms;
    slot.variant = renderVariant(e->_pipe, forms);
    return slot.variant;
}

@implementation RMRenderEncoder (Cubic)

- (void)prepareDrawnRecord
{
    raw::Pipeline *base = !_cb->_encRefused && _pipe ? _pipe->_raw : nullptr;
    _formVariant = _sampledVariant = nullptr;
    _sampledSlots = {};
    if (!base || !(_cubicMask[0] | _cubicMask[1])) {
        _forms.cubic.samplers.clear();
        _forms.cubic.textures.clear();
        _cubicTextures[0] = {};
        _cubicTextures[1] = {};
    } else {
        cubicBinds(self, *base, _forms.cubic);
    }
    _forms.written.clear();
    _forms.fragmentTests = 0;
    _forms.xrSlots = 0;
    _forms.twoPlane = _xrPlanes;
    if (!base) {
        _formSamplers[0] = _formSamplers[1] = 0;
        return;
    }
    // the pass's depth/stencil attachment the draw is tested against: its own, or the hidden one a pass of none is given
    // where a state set in it tests depth or stencil (raw_depth_stencil_state.h testsHiddenZeta)
    const uint32_t zeta = _zeta ? _zeta : _cb->_passes[_passAt].testsHiddenZeta ? raw::kZetaFormat : 0;
    _forms.fragmentTests =
        raw::drawnTests(base->fetchBits && base->depthFormat && !base->storageBits, base->rec->shaderTests, _ds, zeta);
    // a Depth16Unorm a draw tests in its shader renders converted (RawMetalRangeForms.mm depth16Tested)
    if ((_forms.fragmentTests && raw::zetaHasDepth(zeta)) || (base->rec->shaderTests.flags & raw::kShaderTestDepth))
        _cb->_passes[_passAt].shaderTestsDepth = true;
    reboundTextures(*base, _tex[1], _forms.written);
    if (!_forms.cubic.empty() || _forms.fragmentTests || _forms.twoPlane || !_forms.written.empty())
        _formVariant = preparedVariant(self, _forms);
    if (_cb->_passes[_passAt].xrPlaneBinds)
        _sampledSlots = xrSampledSlots(*base, _tex);
    _forms.xrSlots = raw::xrPairSlots(_sampledSlots[1]);
    if (_forms.xrSlots)
        _sampledVariant = preparedVariant(self, _forms);
}

- (const raw::Pipeline *)drawnRecord
{
    raw::Pipeline *base = _pipe->_raw;
    auto *variant = _formVariant;
    std::array<raw::StageTextureMask, 2> standIns = {_sampledSlots[0], raw::pastXrPairSlots(_sampledSlots[1])};
    if (_cb->_passes[_passAt].xrPlaneBinds) {
        const std::array<raw::StageTextureMask, 2> srgb = xrSampledSlots(*base, _tex, true);
        for (uint32_t stage = 0; stage < 2; stage++)
            raw::forEachSlot(srgb[stage], [&](uint32_t slot) { raw::addSlot(standIns[stage], slot); });
    }
    uint16_t drawnSlots = 0;
    if (_sampledVariant) {
        if (_sampledVariant->state() == raw::Variant<raw::Pipeline>::State::Ready) {
            variant = _sampledVariant;
            drawnSlots = _forms.xrSlots;
        } else {
            standIns[1] = _sampledSlots[1];
        }
    }
    if (raw::anySlot(standIns[0]) || raw::anySlot(standIns[1]))
        rmPipelineVeto(_cb, VetoShadows);
    for (uint32_t stage = 0; stage < 2; stage++)
        raw::forEachSlot(standIns[stage], [&](uint32_t slot) {
            _cb->_xrStandIns.insert(_tex[stage][slot]);
        });
    rmXrShadowDraw(_cb, _pipe, *base, variant, _forms.cubic, _forms.fragmentTests, _forms.twoPlane, drawnSlots,
                   _forms.written);
    if (base != _drawn || variant != _drawnVariant) {
        Cmd state = cmd(OpPipeline);
        state.obj = (__bridge void *)_pipe;
        state.obj2 = variant;
        [_cb record:state];
        _drawn = base;
        _drawnVariant = variant;
    }
    return base;
}

@end
