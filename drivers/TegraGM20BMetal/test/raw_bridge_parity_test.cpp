// SPDX-License-Identifier: MIT
// What the raw path took over from the bridge, against the bridge: the m2v device's Metal description
// (nmb_device_pixel_format_block) is raw/runtime/raw_metal_formats.h's row and its linear row
// (nmb_device_linear_bytes_per_row) is refused exactly where the raw device answers none; every capability query the
// bridge installed on m2v's device class (nmb_device_answer_selector) has an answer of its kind in raw_device_answers.h, so
// the raw device opens m2v for none of them. The m2v device is the host bridge over NVK's host twin and hostsim's mock
// GM20B.
#include "bridge/NativeMetalObjects.h"
#include "../raw/runtime/raw_device_answers.h"
#include "../raw/runtime/raw_metal_formats.h"

#include <gtest/gtest.h>
#include <climits>
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

}  // namespace
