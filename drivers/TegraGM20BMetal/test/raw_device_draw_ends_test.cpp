// SPDX-License-Identifier: MIT
// What raw::Device names a draw it refuses at encode (raw_gpu.h Refusal), each id one ending of RawMetalDrawErrors.mm
// drawFailed: a storage form in a pass of another sample count (the client's) or of another texel size (the driver's),
// a shadow variant in a pass with no shadow, a sampler header the range did not make, and a shader depth/stencil test
// of an aspect or sample count the pass lacks (the client's) or of a Depth16Unorm its pass left unconverted (the
// driver's). A planar texture sampled by comparison draws through its sampler's compare header under the texture's map.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_fragment_tests.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_shader_tests.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BTexture.h"
#include <functional>

namespace {

using Header = std::array<uint32_t, 8>;

constexpr uint32_t kRGBA8 = 70, kRGBA16Float = 115, kBGR10XR = 554, kDepth16 = 250, kDepth32Float = 252, kTriangle = 3;
constexpr uint32_t kR8Uint = 13, kR32Float = 100;  // VkFormat
constexpr uint32_t kR32UintWord0 = 0x6014920f;     // a storage view of 4-byte texels (raw_device_layered_fetch_test)
constexpr uint32_t kShadowWord = 5;

uint32_t storageView(uint32_t vkFormat)
{
    const uint32_t identity[4] = {};
    uint32_t word0 = 0;
    return switch_gm20b_view_word0(vkFormat, identity, true, &word0) ? word0 : 0;
}

raw::RuntimeRecord recordOf(uint64_t key, const char *fields, bool cbufs = false)
{
    raw::RuntimeRecord r;
    r.keyString = fields;
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        if (cbufs)
            p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
    }
    r.pipeline.keyHash = key;
    return r;
}

// A state fetching colour 0 through its storage form, a view of 4-byte texels
raw::RuntimeRecord storageForm(uint64_t key, const char *fields)
{
    raw::RuntimeRecord r = recordOf(key, fields);
    raw::PackPipeline &pp = r.pipeline;
    raw::setEntryCount(pp, 1);
    pp.entry[0] = {0, raw::EntryFetch, 2, 0, 0, 0, 0};
    pp.flags = raw::PipelineFetches | raw::PipelineStorageMetadata;
    pp.storageMask = pp.storageWriteMask = 1;
    pp.storageSamples = 1;
    pp.storageTic[0] = kR32UintWord0;
    r.blob = {0x1224, 1, 0x1a00, 0};
    return r;
}

// The storage form of a state whose program runs the depth test on its stand-in (its draws' tests DepthWrite)
raw::RuntimeRecord depthTested(uint64_t key)
{
    raw::RuntimeRecord r = storageForm(key, "samples=1 c0=70 ds=252/0 ");
    raw::PackPipeline &pp = r.pipeline;
    raw::setEntryCount(pp, 2);
    pp.entry[0] = {0, raw::EntryDepthTest, 2, 0, 0, 0, 0};
    pp.entry[1] = {4, raw::EntryFetch, 2, 0, 0, 0, 0};
    pp.flags |= raw::PipelinePushMetadata;
    pp.setBytes = 16;
    pp.shaderTests = {1, 0, raw::kShaderTestDepth, 0, storageView(kR32Float), 0, 0};
    pp.storageTests = uint8_t(raw::FragmentTests::DepthWrite);
    pp.pushMask = 0xfff;
    r.blob = {0x1224, 2, 0x1a00, 0};
    return r;
}

// A shader-side stencil's record of a state with a stencil attachment (raw_shader_test_push.h shadowStencil)
raw::RuntimeRecord shadowStencil(uint64_t key)
{
    raw::RuntimeRecord r = recordOf(key, "samples=1 c0=70 ds=260/260 ");
    raw::PackPipeline &pp = r.pipeline;
    pp.flags = raw::PipelinePushMetadata;
    pp.setBytes = 16;
    raw::setEntryCount(pp, 1);
    pp.entry[0] = {0, raw::EntryStencilTest, 2, 0, 0, 0, 0};
    pp.shaderTests = {0, 0, raw::kShaderTestStencil, uint8_t(kShadowWord), 0, storageView(kR8Uint), 0};
    pp.pushMask = 0x30;
    r.blob = {0x1224, 2, 0x1a00, 0x1111};
    return r;
}

// A state sampling fragment texture 0 through fragment sampler 0, by comparison where `compares`
raw::RuntimeRecord sampling(uint64_t key, bool compares)
{
    raw::RuntimeRecord r = recordOf(key, "samples=1 c0=70 ds=0/0 sampler", true);
    raw::PackPipeline &pp = r.pipeline;
    raw::setEntryCount(pp, 1);
    pp.entry[0] = {0, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, uint8_t(compares ? raw::kEntryCompares : 0)};
    pp.setBytes = 32;
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DeviceDrawEnds : public harness::DeviceTest {
protected:
    raw::Texture target, image;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target, false, true));
        ASSERT_TRUE(dev.createTexture(16, 16, kRGBA8, image));
    }

    raw::Pipeline *made(const raw::RuntimeRecord &r)
    {
        std::string why;
        raw::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    struct Drawn {
        bool drawn = false;
        std::string why;
        raw::Refusal refusal;
        uint32_t handle = 0;  // set 0's first word
    };

    Drawn draw(raw::Pipeline *pl, const raw::PassDesc &pass, const std::function<void(raw::RenderEncoder &)> &bind = {})
    {
        Drawn d;
        raw::CommandBuffer *cb = dev.begin();
        raw::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e) {
            if (cb)
                cb->abandon();
            d.why = "no pass";
            return d;
        }
        e->setPipeline(pl);
        if (bind)
            bind(*e);
        const char *why = nullptr;
        d.drawn = e->draw(kTriangle, 0, 3, 1, 0, &why);
        if (!d.drawn) {
            d.why = why ? why : "refused";
            d.refusal = cb->refusal;
            cb->abandon();
            return d;
        }
        if (const uint8_t *set = raw::DeviceHarness::upload(dev, raw::DeviceHarness::preparedSet(*e), 4))
            memcpy(&d.handle, set, 4);
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno))
            cb->abandon();
        return d;
    }

    static raw::PassDesc onto(raw::Texture &colour, raw::Texture *zeta = nullptr, uint8_t storage = 0)
    {
        raw::PassDesc p;
        p.color[0].texture = &colour;
        p.count = 1;
        p.storageMask = storage;
        p.depthStencil.texture = zeta;
        return p;
    }
};

TEST_F(DeviceDrawEnds, AStorageFormInAPassOfAnotherSampleCountIsTheClientsAndOfAnotherTexelSizeTheDrivers)
{
    raw::Pipeline *fetching = made(storageForm(0xde01, "samples=1 c0=70 ds=0/0 "));
    raw::Pipeline *half = made(storageForm(0xde02, "samples=1 c0=115 ds=0/0 "));
    ASSERT_NE(fetching, nullptr);
    ASSERT_NE(half, nullptr);
    ASSERT_EQ(fetching->storageBits, 1);
    EXPECT_TRUE(draw(fetching, onto(target, nullptr, 1)).drawn) << "one sample of four-byte texels: its own form";
    raw::Texture four, wide;
    ASSERT_TRUE(dev.createStoredMultisample(64, 64, kRGBA8, 4, four));
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA16Float, wide, false, true));
    const Drawn samples = draw(fetching, onto(four, nullptr, 1));
    EXPECT_FALSE(samples.drawn);
    EXPECT_EQ(samples.refusal.id, raw::RefStorageSamples) << samples.why;
    EXPECT_EQ(samples.refusal.c, 4u);
    const Drawn texels = draw(half, onto(wide, nullptr, 1));
    EXPECT_FALSE(texels.drawn);
    EXPECT_EQ(texels.refusal.id, raw::RefStorageView) << texels.why;
    EXPECT_EQ(texels.refusal.b, kRGBA16Float);
}

TEST_F(DeviceDrawEnds, AShadowVariantInAPassWithNoShadowIsTheDrivers)
{
    raw::RuntimeRecord r = recordOf(0xde03, "samples=1 c0=554 ds=0/0 ");
    r.pipeline.flags = raw::PipelinePushMetadata;
    r.pipeline.setBytes = 16;
    raw::setEntryCount(r.pipeline, 1);
    r.pipeline.entry[0] = {0, raw::EntryXrShadow, 2, 0, 0, 0, 0};
    r.blob = {0x1a00, 0x1111};
    raw::Pipeline *pl = made(r);
    ASSERT_NE(pl, nullptr);
    raw::Texture extended;
    ASSERT_TRUE(dev.createTexture(32, 32, kBGR10XR, extended));
    const Drawn d = draw(pl, onto(extended));
    EXPECT_FALSE(d.drawn);
    EXPECT_EQ(d.refusal.id, raw::RefXrShadow) << d.why;
}

// Repeat on both axes, which a converted 4:2:0 texture's address map (1) clamps to the edge
TEST_F(DeviceDrawEnds, APlanarTextureSampledByComparisonTakesItsSamplersCompareHeaderUnderTheMap)
{
    raw::SamplerDesc desc;
    desc.sAddress = desc.tAddress = 2;
    desc.compare = 1;
    raw::Sampler s;
    ASSERT_TRUE(dev.createSampler(desc, s));
    ASSERT_TRUE(dev.samplerVariant(s, 1));
    ASSERT_NE(s.compareMapped[1], 0u);
    EXPECT_NE(s.compareMapped[1], s.compareTsc);
    EXPECT_NE(s.compareMapped[1], s.mapped[1]);
    raw::Pipeline *pl = made(sampling(0xde10, true));
    ASSERT_NE(pl, nullptr);
    raw::Texture planar = image;
    planar.addressMap = 1;
    const Drawn d = draw(pl, onto(target), [&](raw::RenderEncoder &e) {
        e.setTexture(0, &planar);
        e.setSampler(0, &s);
    });
    ASSERT_TRUE(d.drawn) << d.why;
    EXPECT_EQ(d.handle >> 20, s.compareMapped[1]);
    EXPECT_EQ(d.handle & 0xFFFFF, planar.tic);
    Header want = raw::DeviceHarness::tscHeader(dev, s.compareTsc);
    want[0] = (want[0] & ~0x1FFu) | (raw::DeviceHarness::tscHeader(dev, s.mapped[1])[0] & 0x1FFu);
    EXPECT_EQ(raw::DeviceHarness::tscHeader(dev, s.compareMapped[1]), want) << "the compare header, the map's modes";
    const Drawn plain = draw(pl, onto(target), [&](raw::RenderEncoder &e) {
        e.setTexture(0, &image);
        e.setSampler(0, &s);
    });
    ASSERT_TRUE(plain.drawn) << plain.why;
    EXPECT_EQ(plain.handle >> 20, s.compareTsc) << "a plain texture: the sampler's own compare header";
}

TEST_F(DeviceDrawEnds, ASamplerHeaderTheRangeDidNotMakeIsTheDrivers)
{
    raw::SamplerDesc desc;
    desc.sAddress = desc.tAddress = 2;
    raw::Sampler s;
    ASSERT_TRUE(dev.createSampler(desc, s));
    raw::Texture planar = image;
    planar.addressMap = 1;
    for (const bool compares : {false, true}) {
        raw::Pipeline *pl = made(sampling(0xde20 + uint64_t(compares), compares));
        ASSERT_NE(pl, nullptr);
        const Drawn d = draw(pl, onto(target), [&](raw::RenderEncoder &e) {
            e.setTexture(0, &planar);
            e.setSampler(0, &s);
        });
        EXPECT_FALSE(d.drawn);
        EXPECT_EQ(d.why, "a planar texture's sampler without its address map");
        EXPECT_EQ(d.refusal.id, raw::RefSamplerHeader);
        EXPECT_EQ(d.refusal.b, 1u);
        EXPECT_EQ(d.refusal.c, compares ? 1u : 0u);
    }
}

TEST_F(DeviceDrawEnds, AShaderTestOfAnAspectOrSampleCountThePassLacksIsTheClientsAndOfAnUnconvertedDepth16TheDrivers)
{
    raw::Pipeline *stencil = made(shadowStencil(0xde30)), *depth = made(depthTested(0xde31));
    ASSERT_NE(stencil, nullptr);
    ASSERT_NE(depth, nullptr);
    raw::Texture depthOnly, depth16, fourDepth;
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depthOnly));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth16, 1, false, depth16));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 4, true, fourDepth));
    raw::DepthStencilDesc less;
    less.depthCompare = 1;
    less.depthWrite = true;
    const raw::DepthStencilState tested(less);
    const auto withTests = [&](raw::RenderEncoder &e) {
        e.setDepthStencil(&tested);
    };
    const Drawn noStencil = draw(stencil, onto(target, &depthOnly), withTests);
    EXPECT_FALSE(noStencil.drawn);
    EXPECT_EQ(noStencil.refusal.id, raw::RefDepthStencil) << noStencil.why;
    const Drawn mixed = draw(depth, onto(target, &fourDepth, 1), withTests);
    EXPECT_FALSE(mixed.drawn);
    EXPECT_EQ(mixed.refusal.id, raw::RefDepthStencil) << mixed.why;
    const Drawn unconverted = draw(depth, onto(target, &depth16, 1), withTests);
    EXPECT_FALSE(unconverted.drawn);
    EXPECT_EQ(unconverted.refusal.id, raw::RefTestsInvariant) << unconverted.why;
    EXPECT_EQ(unconverted.refusal.a, kDepth16);
}

}  // namespace
