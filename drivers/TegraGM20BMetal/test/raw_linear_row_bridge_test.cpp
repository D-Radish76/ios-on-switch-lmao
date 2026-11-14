// SPDX-License-Identifier: MIT
// raw/runtime/raw_metal_formats.h against the bridge it was ported from, over every format value: the m2v device's Metal
// description (nmb_device_pixel_format_block) is the table's row, and its linear row (nmb_device_linear_bytes_per_row) is
// refused exactly where the raw device answers no row (raw_device_answers.h's linear alignment). The m2v device is the
// host bridge over NVK's host twin and hostsim's mock GM20B.
#include "bridge/NativeMetalObjects.h"
#include "../raw/runtime/raw_device_answers.h"
#include "../raw/runtime/raw_metal_formats.h"

#include <gtest/gtest.h>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace {

namespace mf = raw::metal_formats;

uint64_t linearAlignment(uint64_t format)
{
    const raw::answers::Kind &k = raw::answers::kThisKind;
    for (size_t i = 0; i < k.argCount; i++)
        if (!strcmp(k.args[i].selector, "minimumLinearTextureAlignmentForPixelFormat:") && format >= k.args[i].first &&
            format <= k.args[i].last)
            return k.args[i].value;
    return 0;
}

TEST(RawLinearRowBridge, TheDescriptionAndTheRowsAreTheBridges)
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

}  // namespace
