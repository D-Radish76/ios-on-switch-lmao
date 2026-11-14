// SPDX-License-Identifier: MIT
// A kernel's sampler entries that hold the texture slots they sample (raw_pack.h kernelSamplerPairs) on raw::Device: each
// binds its header under the address map of its own textures (raw_sampler_map.h), a constexpr one through the variants the
// device made with it. A kernel sampling a 4:2:0 YCbCr texture through a constexpr sampler beside an RGBA texture went to
// m2v (compute.image, TermConvertedSampler): one map for the whole kernel serves only one of them.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_compute_record.h"

#include <array>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint32_t kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0));
constexpr uint8_t kConstexpr = 0x80;
constexpr uint32_t kKnown = 1u << 31;
// TEXSAMP0 ADDRESS_U, _V and _P (clb197tex.h): all WRAP, all CLAMP_TO_EDGE, all BORDER
constexpr uint32_t kAddress = 0x1ff, kWrap = 0, kEdge = 2 | 2 << 3 | 2 << 6, kBorder = 3 | 3 << 3 | 3 << 6;

std::array<uint32_t, 8> header(uint32_t address)
{
    return {0x26000 | address, 0x291, 0xfff000, 0, 0, 0, 0, 0};
}

// Textures 0 and 1 each sampled through a combined descriptor of its own (set-0 words 0 and 1), both samplers constexpr
// of one header; `pairs0` and `pairs1` their windows
raw::ComputeRecord kernel(uint32_t address, uint32_t pairs0, uint32_t pairs1)
{
    raw::ComputeRecord r;
    r.code.assign(64, 0);
    r.cbufType[0] = raw::CbufRoot;
    r.cbufs = 1;
    r.kernel.regionWord = -1;
    r.kernel.qmd[kQmdCbufValid] |= 1;
    r.kernel.setBytes = 8;
    r.kernel.entries = 4;
    r.kernel.entry[0] = {0, raw::KernelTexture, 0, 0};
    r.kernel.entry[1] = {0, raw::KernelSampler, kConstexpr, pairs0};
    r.kernel.entry[2] = {4, raw::KernelTexture, 1, 0};
    r.kernel.entry[3] = {4, raw::KernelSampler, kConstexpr, pairs1};
    r.kernel.readOnlyEntries = 0xf;
    r.samplers = {header(address)};
    return r;
}

class DeviceKernelPairs : public harness::DeviceTest {
protected:
    raw::Texture converted, rgba;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(8, 8, kRGBA8, converted));
        ASSERT_TRUE(dev.createTexture(8, 8, kRGBA8, rgba));
        converted.addressMap = 3;  // a 4:2:0 texture read as codes (raw_ycbcr.h YcbcrPlan::addressMap)
    }

    // One dispatch of `r`, the converted texture at slot 0 and the RGBA one at 1: its two set-0 words, none where it was
    // refused (`why`)
    std::vector<uint32_t> words(const raw::ComputeRecord &r, std::string &why)
    {
        const raw::ComputeState *k = raw::placeCompute(dev, r);
        EXPECT_NE(k, nullptr);
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!k || !cb)
            return {};
        std::vector<uint32_t> out;
        {
            raw::ComputeEncoder e(*cb, dev);
            e.setState(k);
            e.setTexture(0, &converted);
            e.setTexture(1, &rgba);
            const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
            const char *refused = nullptr;
            if (e.dispatch(groups, threads, &refused)) {
                const uint32_t *set = raw::DeviceHarness::kernelSet(e);
                if (set)
                    out.assign(set, set + 2);
            } else {
                why = refused ? refused : "?";
            }
            e.end();
        }
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return out;
    }

    uint32_t address(uint32_t word)
    {
        return raw::DeviceHarness::samplerHeader(dev, word >> 20)[0] & kAddress;
    }
};

TEST_F(DeviceKernelPairs, EachSamplerTakesTheMapOfTheTextureItSamples)
{
    static const raw::ComputeRecord r = kernel(kWrap, kKnown | 1u << 0, kKnown | 1u << 1);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(set[0] & 0xfffff, converted.tic);
    EXPECT_EQ(set[1] & 0xfffff, rgba.tic);
    EXPECT_EQ(address(set[0]), kEdge) << "the YCbCr texture's repeat reads clamp to its edge";
    EXPECT_EQ(address(set[1]), kWrap) << "the RGBA texture's sampler keeps its own header";
    EXPECT_EQ(raw::DeviceHarness::samplerHeader(dev, set[0] >> 20)[1], header(kWrap)[1]);
}

TEST_F(DeviceKernelPairs, ASamplerOfTheNullImageOrNoConvertedTextureKeepsItsHeader)
{
    static const raw::ComputeRecord r = kernel(kBorder, kKnown, kKnown | 1u << 1);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(address(set[0]), kBorder);
    EXPECT_EQ(address(set[1]), kBorder);
}

TEST_F(DeviceKernelPairs, ARecordWithoutPairsKeepsTheOldRule)
{
    static const raw::ComputeRecord r = kernel(kWrap, 0, 0);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(address(set[0]), kWrap) << "a constexpr sampler without pairs is bound unmapped (admission refuses it)";
    EXPECT_EQ(address(set[1]), kWrap);
}

TEST_F(DeviceKernelPairs, OneSamplerOfTexturesUnderOneHeaderTakesIt)
{
    rgba.addressMap = 1;  // repeat to the edge as map 3 sends it: the same header
    static const raw::ComputeRecord r = kernel(kWrap, kKnown | 3u, kKnown | 3u);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(address(set[0]), kEdge);
    EXPECT_EQ(set[0] >> 20, set[1] >> 20);
}

TEST_F(DeviceKernelPairs, OneSamplerOfTexturesUnderTwoHeadersIsNoDispatch)
{
    static const raw::ComputeRecord r = kernel(kWrap, kKnown | 3u, kKnown | 1u << 1);
    std::string why;
    EXPECT_TRUE(words(r, why).empty());
    EXPECT_EQ(why, "a kernel sampler that samples textures whose address maps give it different headers");
}

TEST_F(DeviceKernelPairs, AConstexprSamplerHasOneHeaderPerDistinctMapping)
{
    static const raw::ComputeRecord wrap = kernel(kWrap, kKnown, kKnown), edge = kernel(kEdge, kKnown, kKnown),
                                    border = kernel(kBorder, kKnown, kKnown);
    for (const raw::ComputeRecord *r : {&wrap, &edge, &border})
        ASSERT_NE(raw::placeCompute(dev, *r), nullptr);
    const auto index = [&](const raw::ComputeRecord &r) {
        return uint32_t(raw::placeCompute(dev, r)->rec->entry[1].slot & 0x7f);
    };
    const uint32_t w = index(wrap), e = index(edge), b = index(border);
    EXPECT_NE(dev.constexprTsc(w, 1), dev.constexprTsc(w, 0));
    EXPECT_EQ(dev.constexprTsc(w, 2), dev.constexprTsc(w, 0)) << "clamps to zero or the border: none in WRAP";
    EXPECT_EQ(dev.constexprTsc(w, 3), dev.constexprTsc(w, 1));
    for (uint8_t map = 1; map < 4; ++map)
        EXPECT_EQ(dev.constexprTsc(e, map), dev.constexprTsc(e, 0)) << unsigned(map);
    EXPECT_EQ(dev.constexprTsc(b, 1), dev.constexprTsc(b, 0));
    EXPECT_NE(dev.constexprTsc(b, 2), dev.constexprTsc(b, 0));
    EXPECT_EQ(dev.constexprTsc(b, 3), dev.constexprTsc(b, 2));
    EXPECT_EQ(raw::DeviceHarness::samplerHeader(dev, dev.constexprTsc(b, 2))[0] & kAddress, kEdge);
}

}  // namespace
