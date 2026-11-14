// SPDX-License-Identifier: MIT
// What the raw path took over from the bridge, against the bridge: the m2v device's Metal description
// (nmb_device_pixel_format_block) is raw/runtime/raw_metal_formats.h's row and its linear row
// (nmb_device_linear_bytes_per_row) is refused exactly where the raw device answers none; every capability query the
// bridge installed on m2v's device class (nmb_device_answer_selector) has an answer of its kind in raw_device_answers.h, so
// the raw device opens m2v for none of them; a sampler's word (raw_sampler_word.h) is switch_nmb_sampler_word's over every
// enumerator value and one past each, and over the level clamps on and around Metal's grid. The m2v device is the host
// bridge over NVK's host twin and hostsim's mock GM20B.
#include "bridge/NativeMetalDirect.h"
#include "bridge/NativeMetalObjects.h"
#include "../raw/runtime/raw_device_answers.h"
#include "../raw/runtime/raw_metal_formats.h"
#include "../raw/runtime/raw_sampler_word.h"

#include <gtest/gtest.h>
#include <cfloat>
#include <climits>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace {

namespace mf = raw::metal_formats;

const raw::answers::ArgRange *argument(const char *selector, uint64_t value = ~0ull)
{
    const raw::answers::Kind &k = raw::answers::kThisKind;
    for (size_t i = 0; i < k.argCount; i++)
        if (!strcmp(k.args[i].selector, selector) &&
            (value == ~0ull || (value >= k.args[i].first && value <= k.args[i].last)))
            return &k.args[i];
    return nullptr;
}

const raw::answers::Count *count(const char *selector)
{
    const raw::answers::Kind &k = raw::answers::kThisKind;
    for (size_t i = 0; i < k.countCount; i++)
        if (!strcmp(k.counts[i].selector, selector))
            return &k.counts[i];
    return nullptr;
}

uint64_t linearAlignment(uint64_t format)
{
    const raw::answers::ArgRange *r = argument("minimumLinearTextureAlignmentForPixelFormat:", format);
    return r ? r->value : 0;
}

TEST(RawBridgeParity, TheDescriptionAndTheLinearRowsAreTheBridges)
{
    const char *nvk = getenv("SWITCH_NVK");
    ASSERT_NE(nvk, nullptr);
    char loader[PATH_MAX];
    ASSERT_NE(realpath(nvk, loader), nullptr);
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    NmbDevice *device = nmb_device_open(loader, nullptr);
    ASSERT_NE(device, nullptr) << nmb_last_error();
    size_t described = 0, rows = 0;
    for (uint32_t format = 0; format < 2048; format++) {
        uint64_t block[4] = {};
        const bool bridge = nmb_device_pixel_format_block(device, format, block);
        const mf::Described *d = mf::described(format);
        EXPECT_EQ(bridge, d != nullptr) << format;
        if (bridge && d) {
            described++;
            EXPECT_EQ(block[0], d->bytes) << format;
            EXPECT_EQ(block[1], d->blockW) << format;
            EXPECT_EQ(block[2], d->blockH) << format;
        }
        const uint64_t bridgeRow = nmb_device_linear_bytes_per_row(device, format, 100);
        const uint64_t rawRow = mf::linearRow(100, mf::texelBytes(format), linearAlignment(format));
        rows += rawRow != 0;
        EXPECT_EQ(bridgeRow != 0, rawRow != 0) << format << ": bridge " << bridgeRow << ", raw " << rawRow;
    }
    EXPECT_EQ(described, std::size(mf::kDescribed));
    EXPECT_GT(rows, 150u);
}

TEST(RawBridgeParity, EveryCapabilityQueryTheBridgeAnsweredIsRaws)
{
    size_t asked = 0;
    uint32_t kind = 0;
    for (size_t i = 0; const char *name = nmb_device_answer_selector(i, &kind); i++) {
        asked++;
        const bool flag = kind == NMB_ANSWER_FLAG || kind == NMB_ANSWER_FLAG_ARG;
        if (kind == NMB_ANSWER_COUNT || kind == NMB_ANSWER_FLAG) {
            const raw::answers::Count *c = count(name);
            ASSERT_NE(c, nullptr) << name;
            EXPECT_EQ(c->flag, flag) << name;
        } else {
            const raw::answers::ArgRange *r = argument(name);
            ASSERT_NE(r, nullptr) << name;
            EXPECT_EQ(r->flag, flag) << name;
        }
    }
    EXPECT_GT(asked, 60u);
}

static_assert(sizeof(raw::SamplerFacts) == sizeof(NmbSamplerDescriptor) &&
                  offsetof(raw::SamplerFacts, lodMin) == offsetof(NmbSamplerDescriptor, lod_min),
              "the bridge's sampler descriptor");

struct SamplerParity {
    size_t compared = 0, differ = 0;

    void check(const raw::SamplerFacts &f)
    {
        NmbSamplerDescriptor d;
        memcpy(&d, &f, sizeof d);
        const uint32_t ours = raw::samplerWord(f), bridge = switch_nmb_sampler_word(&d);
        compared++;
        if (ours != bridge && ++differ <= 8)
            ADD_FAILURE() << std::hex << "raw " << ours << ", bridge " << bridge << std::dec << ": filters "
                          << f.minFilter << "/" << f.magFilter << "/" << f.mipFilter << " address " << f.sAddress << "/"
                          << f.tAddress << "/" << f.rAddress << " border " << f.borderColor << " compare " << f.compare
                          << " normalized " << f.normalized << " anisotropy " << f.maxAnisotropy << " lod " << f.lodMin
                          << "-" << f.lodMax;
    }
};

TEST(RawBridgeParity, TheSamplerWordIsTheBridgesOverEveryEnumerator)
{
    SamplerParity p;
    raw::SamplerFacts f{};
    f.lodMax = FLT_MAX;
    for (f.minFilter = 0; f.minFilter < 4; f.minFilter++)
        for (f.magFilter = 0; f.magFilter < 4; f.magFilter++)
            for (f.mipFilter = 0; f.mipFilter < 4; f.mipFilter++)
                for (f.sAddress = 0; f.sAddress < 7; f.sAddress++)
                    for (f.tAddress = 0; f.tAddress < 7; f.tAddress++)
                        for (f.rAddress = 0; f.rAddress < 7; f.rAddress++)
                            for (f.borderColor = 0; f.borderColor < 4; f.borderColor++)
                                for (f.compare = 0; f.compare < 9; f.compare++)
                                    for (f.normalized = 0; f.normalized < 2; f.normalized++)
                                        for (uint64_t a : {0ull, 1ull, 2ull, 16ull, (1ull << 32) | 1})
                                            f.maxAnisotropy = a, p.check(f);
    f = {};
    for (uint64_t past : {1ull << 32, ~0ull}) {
        f.minFilter = past;
        p.check(f);
        f.minFilter = 0;
        f.compare = past;
        p.check(f);
        f.compare = 0;
    }
    EXPECT_EQ(p.differ, 0u);
    EXPECT_GT(p.compared, 7000000u);
}

TEST(RawBridgeParity, TheSamplerWordIsTheBridgesOverTheLevelClamps)
{
    std::vector<float> clamps = {NAN, -INFINITY, INFINITY, -FLT_MAX, FLT_MAX, -0.0f, FLT_MIN, 1e-9f, 1e9f};
    for (int k = -256; k <= 70 * 256; k++)
        clamps.push_back(float(k) / 256.0f);
    for (float c = 0.49f; c < 15.6f; c += 1.0f)
        clamps.insert(clamps.end(), {std::nextafter(c + 0.01f, 0.0f), c + 0.01f, std::nextafter(c + 0.01f, 99.0f)});
    SamplerParity p;
    raw::SamplerFacts f{};
    for (uint64_t mip = 0; mip < 3; mip++) {
        f.mipFilter = mip;
        for (float c : clamps) {
            f.lodMin = c, f.lodMax = FLT_MAX;
            p.check(f);
            f.lodMin = 0, f.lodMax = c;
            p.check(f);
        }
    }
    for (float lo : {NAN, 0.0f, 0.5f, 0.515625f, 7.25f, 64.0f})
        for (float hi : {NAN, 0.0f, 0.5f, 0.515625f, 14.99f, 15.0f, INFINITY})
            f.lodMin = lo, f.lodMax = hi, p.check(f);
    EXPECT_EQ(p.differ, 0u);
}

}  // namespace
