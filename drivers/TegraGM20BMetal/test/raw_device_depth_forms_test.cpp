// SPDX-License-Identifier: MIT
// Depth/stencil and turned forms on raw::Device: a Depth16Unorm converted a layer a pass and moved into a turned
// pass's plane, a separate pair's aspects of any zeta format, and a stand-in drawn as a turned target.
#include "raw_device_forms.h"
#include "../raw/runtime/raw_mme.h"
#include "../raw/runtime/raw_zeta_colour.h"

namespace {

// A Depth16Unorm of three layers into a Depth32Float_Stencil8 scratch of three and back: a pass a layer, each into that
// layer, each reading its layer of the source
TEST_F(DevicePassForms, ALayeredDepthConversionIsAPassALayer)
{
    ownState(raw::turnDepthState(raw::kZetaFormat), "samples=1 ds=260/260 ");
    ownState(raw::turnDepthState(kDepth16), "samples=1 ds=250/0 ");
    raw::Texture d16;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth16, 1, true, d16, 3, raw::fx::kTexTwoDArray));
    ASSERT_NE(d16.tic, 0u);
    for (const bool back : {false, true}) {
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        raw::Texture *scratch = cb->turnScratch(raw::kZetaFormat, 32, 32, raw::kDepthConvertTag, 1, 3);
        ASSERT_NE(scratch, nullptr);
        ASSERT_EQ(scratch->layers, 3u);
        const char *why = "";
        ASSERT_TRUE(back ? cb->depthConvert(d16, *scratch, &why) : cb->depthConvert(*scratch, d16, &why)) << why;
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        const auto ms = submitted(first);
        const raw::Texture &target = back ? d16 : *scratch;
        std::vector<uint64_t> targets;
        uint32_t upper = 0;
        for (const Method &x : ms)
            if (x.subc == raw::SUBC_3D && x.method == NVB197_SET_ZT_A)
                upper = x.value;
            else if (x.subc == raw::SUBC_3D && x.method == NVB197_SET_ZT_A + 4)
                targets.push_back(uint64_t(upper) << 32 | x.value);
        const std::vector<uint64_t> want = {target.va, target.va + target.layerBytes, target.va + 2 * target.layerBytes};
        EXPECT_EQ(targets, want) << (back ? "back" : "in");
        for (const uint32_t c : values(ms, NVB197_SET_ZT_SIZE_C))
            EXPECT_EQ(c, 1u);
    }
}

// A Depth16Unorm a draw's shader tests beside a turned target, as the range encode hands it down (RawMetalRangeForms.mm
// depth16Tested): converted into a Depth32Float_Stencil8 scratch of its frame, which the turned pass moves into a stand-in
// of its plane and back, then converted back. Four draws (two conversions, two turn copies through the zeta's colour route)
// and four copies (each turn copy's pool image filled and drained); a conversion straight into the plane would be two
// draws and no copies
TEST_F(DevicePassForms, ATurnedDepth16ShaderTestedPassMovesItsConvertedScratch)
{
    ownState(raw::turnDepthState(raw::kZetaFormat), "samples=1 ds=260/260 ");
    ownState(raw::turnDepthState(kDepth16), "samples=1 ds=250/0 ");
    ownState(raw::stencilColourState(raw::kZetaFormat, 1, raw::StencilSample::Zero), "samples=1 c0=103 ");
    raw::Texture target, d16;
    ASSERT_TRUE(dev.createTexture(48, 32, kRGBA8, target, false, true));
    target.rotation = 1;
    ASSERT_TRUE(dev.createOwnedZeta(32, 48, kDepth16, 1, true, d16));
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    raw::Texture *scratch = cb->turnScratch(raw::kZetaFormat, 32, 48, raw::kDepthConvertTag);
    ASSERT_NE(scratch, nullptr);
    const char *why = "";
    ASSERT_TRUE(cb->depthConvert(*scratch, d16, &why)) << why;
    raw::PassDesc p;
    p.color[0] = kept(target, raw::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {scratch, false, true, 1.0f, 0};
    p.biasDepthBits = 16;
    NoDraws draws;
    raw::TurnedPass turned;
    raw::RenderEncoder *e = nullptr;
    ASSERT_TRUE(turned.begin(*cb, p, draws, &why)) << why;
    ASSERT_NE(e = cb->render(p, &why), nullptr) << why;
    e->end();
    ASSERT_TRUE(turned.end(*cb, &why)) << why;
    ASSERT_TRUE(cb->depthConvert(d16, *scratch, &why)) << why;
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const auto ms = submitted(first);
    std::vector<uint64_t> zetas;
    uint32_t upper = 0;
    size_t draws4 = 0;
    for (const Method &x : ms) {
        if (x.subc != raw::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_ZT_A)
            upper = x.value;
        else if (x.method == NVB197_SET_ZT_A + 4)
            zetas.push_back(uint64_t(upper) << 32 | x.value);
        else if (x.method == NVB197_CALL_MME_MACRO(raw::mme::kDraw))
            draws4++;
    }
    ASSERT_EQ(zetas.size(), 3u) << "the conversion in, the pass, the conversion back";
    EXPECT_EQ(zetas[0], scratch->va);
    EXPECT_NE(zetas[1], scratch->va) << "the pass renders a stand-in of its plane";
    EXPECT_NE(zetas[1], d16.va);
    EXPECT_EQ(zetas[2], d16.va);
    EXPECT_EQ(draws4, 4u);
    const auto cs = copies(ms);
    EXPECT_EQ(cs.size(), 4u);
    EXPECT_EQ(touching(cs, scratch->va, scratch->layerBytes), 2u) << "the turn copy back's pool image";
    EXPECT_EQ(touching(cs, zetas[1], 1), 2u) << "the turn copy in's pool image";
}

// A separate pair of a 64x64 Depth32Float and a 32x32 Stencil8: the render area is the smaller, as an M4 Max counts the
// stencil (m2v-passes3 p3probe pair_sizes)
TEST_F(DevicePassForms, ASeparatePairsStencilBoundsTheRenderArea)
{
    raw::Texture colour, depth, stencil;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depth));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, 253, 1, false, stencil));
    raw::PassDesc p;
    p.color[0] = kept(colour, raw::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil.texture = &depth;
    p.separate = {&depth, &stencil, true, true, true, true};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    EXPECT_EQ(lastOf(ms, NVB197_SET_SURFACE_CLIP_HORIZONTAL) >> 16, 32u);
    EXPECT_EQ(lastOf(ms, NVB197_SET_SURFACE_CLIP_VERTICAL) >> 16, 32u);
}

// A Depth32Float depth beside a Depth32Float_Stencil8 texture's stencil: the stencil byte moves straight between the two
// 8-byte texels (bytes 4 and 5, four 2-byte components a side, the third written), no two-byte temporary
TEST_F(DevicePassForms, AStencilAspectOfADepth32FloatStencil8TextureMovesStraight)
{
    raw::Texture colour, depth, stencil;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, false, depth));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, raw::kZetaFormat, 1, false, stencil));
    raw::PassDesc p;
    p.color[0] = kept(colour, raw::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil.texture = &depth;
    p.separate = {&depth, &stencil, false, true, false, true};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    const uint64_t backing = addressOf(ms, NVB197_SET_ZT_A);
    constexpr uint32_t kStencilWord = 6 | 6 << 4 | 2 << 8 | 6 << 12 | 1 << 16 | 3 << 20 | 3 << 24;
    const auto cs = copies(ms);
    ASSERT_EQ(copiesBetween(cs, stencil.va, backing), 1u);
    ASSERT_EQ(copiesBetween(cs, backing, stencil.va), 1u);
    for (const Copy &c : cs)
        if (c.from == stencil.va || c.to == stencil.va)
            EXPECT_EQ(c.remap, kStencilWord);
}

// A block-linear colour image marked turned, as a stand-in of a turned target of a format the GM20B renders none of is made
// (in its texture's memory shape, its rotation): the turned pass draws into it in place, through the quarter turn's swizzle
TEST_F(DevicePassForms, AStandInMarkedTurnedIsDrawnTurnedInPlace)
{
    raw::Texture standIn;
    ASSERT_TRUE(dev.createTexture(48, 32, kRGBA8, standIn, false, true));
    standIn.rotation = 1;
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    raw::PassDesc p;
    p.color[0] = kept(standIn, raw::ColorAttachment::Clear);
    p.count = 1;
    NoDraws draws;
    raw::TurnedPass turned;
    const char *why = "";
    raw::RenderEncoder *e = nullptr;
    ASSERT_TRUE(turned.begin(*cb, p, draws, &why)) << why;
    ASSERT_NE(e = cb->render(p, &why), nullptr) << why;
    e->end();
    ASSERT_TRUE(turned.end(*cb, &why)) << why;
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const auto ms = submitted(first);
    EXPECT_EQ(addressOf(ms, NVB197_SET_COLOR_TARGET_A(0)), standIn.va);
    const std::vector<uint32_t> swizzles = values(ms, NVB197_SET_VIEWPORT_COORDINATE_SWIZZLE(0));
    EXPECT_NE(std::find(swizzles.begin(), swizzles.end(), raw::kSwizzleExchange), swizzles.end());
    EXPECT_TRUE(copies(ms).empty()) << "nothing moves: the stand-in is the pass's turned target";
}

}  // namespace
