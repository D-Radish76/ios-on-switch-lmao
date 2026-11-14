// SPDX-License-Identifier: MIT
// raw/runtime/raw_kernel_pairs.h and raw_sampler_map.h: the texture slots the direct compile records for a kernel's sampler
// entry (a combined descriptor's own image; a heap sampler's unknown), a sampler header under a converted texture's
// address map as the header NVK packs of the mapped modes, and the one map a sampler entry takes of its textures.
#include <gtest/gtest.h>

#include "../../../kits/SwitchGM20B/SwitchGM20BSampler.h"
#include "../raw/runtime/raw_kernel_pairs.h"
#include "../raw/runtime/raw_sampler_map.h"

#include <cstring>
#include <map>

namespace {

constexpr uint32_t kKnown = raw::kSamplerPairsKnown;

// The VkSamplerAddressMode metal2vulkan creates of an MTLSamplerAddressMode
uint32_t vulkanMode(uint8_t metal)
{
    static const uint32_t modes[] = {2, 4, 0, 1, 3, 3};
    return modes[metal];
}

// NVK's header of a linear sampler of `modes` (MTLSamplerAddressMode of s, t, r), a transparent black border
void nvkHeader(const uint8_t (&modes)[3], uint32_t w[8])
{
    switch_gm20b_sampler_state s = {};
    s.mag_filter = s.min_filter = SWITCH_GM20B_FILTER_LINEAR;
    s.max_lod = 1000.0f;
    s.compare_op = SWITCH_GM20B_COMPARE_ALWAYS;
    for (int i = 0; i < 3; ++i)
        s.address[i] = vulkanMode(modes[i]);
    ASSERT_TRUE(switch_gm20b_sampler_header(&s, w));
}

TEST(KernelPairs, ACombinedSamplerSamplesItsOwnImage)
{
    EXPECT_EQ(raw::directSamplerWindow(true, true, 2), kKnown | 1u << 2);
    EXPECT_EQ(raw::directSamplerWindow(true, true, 30), kKnown | 1u << 30);
    EXPECT_EQ(raw::directSamplerWindow(true, false, 0), kKnown) << "the null image: no slot";
}

TEST(KernelPairs, AHeapSamplersOrAPastSlotsPairsAreUnknown)
{
    EXPECT_EQ(raw::directSamplerWindow(false, false, 0), 0u);
    EXPECT_EQ(raw::directSamplerWindow(false, true, 3), 0u);
    EXPECT_EQ(raw::directSamplerWindow(true, true, 31), 0u);
}

TEST(KernelPairs, TheWindowReadsBackAsItsSlots)
{
    const raw::PackKernelEntry known = {4, raw::KernelSampler, 0x80, raw::directSamplerWindow(true, true, 5)};
    EXPECT_TRUE(raw::kernelSamplerPairsKnown(known));
    EXPECT_EQ(raw::kernelSamplerPairs(known), 1u << 5);
    const raw::PackKernelEntry old = {4, raw::KernelSampler, 0x80, 0};
    EXPECT_FALSE(raw::kernelSamplerPairsKnown(old));
}

TEST(SamplerMap, AMappedHeaderIsTheHeaderOfTheMappedModes)
{
    for (uint8_t s = 0; s < 6; ++s)
        for (uint8_t t = 0; t < 6; ++t)
            for (uint8_t map = 0; map < 4; ++map) {
                const uint8_t modes[3] = {s, t, uint8_t((s + t) % 6)};
                const uint8_t mapped[3] = {raw::mappedAddressMode(modes[0], map), raw::mappedAddressMode(modes[1], map),
                                           raw::mappedAddressMode(modes[2], map)};
                uint32_t own[8], want[8], got[8];
                nvkHeader(modes, own);
                nvkHeader(mapped, want);
                raw::mapSamplerHeader(own, map, got);
                EXPECT_EQ(memcmp(got, want, sizeof got), 0)
                    << "modes " << unsigned(s) << unsigned(t) << " map " << unsigned(map) << ": " << std::hex << got[0]
                    << " for " << want[0];
            }
}

TEST(SamplerMap, MapsSendWrappingAndClampingModesToTheEdge)
{
    EXPECT_EQ(raw::mappedAddressMode(2, 1), 0) << "repeat";
    EXPECT_EQ(raw::mappedAddressMode(4, 1), 4) << "clamp to zero kept by a 4:4:4 map";
    EXPECT_EQ(raw::mappedAddressMode(4, 2), 0);
    EXPECT_EQ(raw::mappedAddressMode(5, 3), 0);
    EXPECT_EQ(raw::mappedAddressMode(2, 0), 2);
    EXPECT_EQ(raw::mappedModes(2, 0, 5, 2), raw::mappedModes(2, 0, 4, 2)) << "both clamps go to the edge";
}

TEST(SamplerMap, ASamplerTakesTheOneMapItsTexturesShare)
{
    const std::map<uint32_t, uint8_t> maps = {{0, 3}, {1, 0}, {2, 1}, {4, 3}};
    const auto mapOf = [&](uint32_t slot) -> uint8_t {
        const auto at = maps.find(slot);
        return at == maps.end() ? 0 : at->second;
    };
    // a repeating sampler: maps 1 and 3 give one header, 0 and 2 its own
    const auto header = [](uint8_t m) {
        return raw::mappedModes(2, 2, 2, m);
    };
    uint8_t map = 9;
    EXPECT_TRUE(raw::pairedSamplerMap(0, header, mapOf, map));
    EXPECT_EQ(map, 0);
    EXPECT_TRUE(raw::pairedSamplerMap(1u << 0, header, mapOf, map));
    EXPECT_EQ(map, 3);
    EXPECT_TRUE(raw::pairedSamplerMap(1u << 0 | 1u << 2 | 1u << 4, header, mapOf, map));
    EXPECT_EQ(map, 3);
    EXPECT_TRUE(raw::pairedSamplerMap(1u << 1 | 1u << 3, header, mapOf, map)) << "unbound slot 3 reads no map";
    EXPECT_EQ(map, 0);
    EXPECT_FALSE(raw::pairedSamplerMap(1u << 0 | 1u << 1, header, mapOf, map));
    const auto edge = [](uint8_t m) {
        return raw::mappedModes(0, 0, 0, m);
    };
    EXPECT_TRUE(raw::pairedSamplerMap(1u << 0 | 1u << 1, edge, mapOf, map)) << "a map that changes nothing";
}

}  // namespace
