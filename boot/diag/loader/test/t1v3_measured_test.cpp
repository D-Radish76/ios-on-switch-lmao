// T1 v3's read phase against the I2C1 and CAR words T1-T measured (boot/diag/erista-t1t-run1.txt). The model holds those
// words still, so a run that passes the gates writes the divisor, reads it back unchanged and stops at 060B.
#include <gtest/gtest.h>

#include <utility>
#include <vector>

extern "C" {
#include "../t1v3.h"
}

extern "C" {
uint32_t switch_t1v3_read32(uint32_t)
{
    return 0;
}
void switch_t1v3_write32(uint32_t, uint32_t) {}
}

namespace {
constexpr uint32_t kTiming0 = 0x00000204u, kTiming1 = 0x04070404u;

struct Still {
    std::vector<std::pair<unsigned, uint32_t>> writes;
    uint64_t clock = 0;

    uint32_t read(unsigned reg) const
    {
        switch (reg) {
        case T1V3_CAR_RST_DEVICES_L: return 0x049102CA;
        case T1V3_CAR_CLK_OUT_ENB_L: return 0x9802D131;
        case T1V3_CAR_CLK_SOURCE_I2C1: return 0x00000003;
        case T1V3_CAR_PLLP_BASE: return 0x48115408;
        case T1V3_I2C1_CNFG: return 0x00002842;
        case T1V3_I2C1_STATUS: return 0;
        case T1V3_I2C1_INT_STATUS: return 0;
        case T1V3_I2C1_CLK_DIVISOR: return 0x00050001;
        case T1V3_I2C1_CONFIG_LOAD: return 0x00000004;
        case T1V3_I2C1_TIMING_0: return kTiming0;
        case T1V3_I2C1_TIMING_1: return kTiming1;
        }
        return 0;
    }
};

uint32_t read_still(void *context, unsigned reg)
{
    return static_cast<Still *>(context)->read(reg);
}

void write_still(void *context, unsigned reg, uint32_t value)
{
    static_cast<Still *>(context)->writes.emplace_back(reg, value);
}

uint64_t now_still(void *context)
{
    return static_cast<Still *>(context)->clock += 10;
}

switch_t1v3_result run(Still &still, uint32_t pin_0, uint32_t pin_1)
{
    switch_t1v3_io io = {&still, pin_0, pin_1, read_still, write_still, now_still};
    switch_t1v3_result result;
    switch_t1v3_run(&io, &result);
    return result;
}
} // namespace

TEST(T1v3Measured, WithoutPinsTheRunStopsAt061AAndWritesNothing)
{
    Still still;
    const auto result = run(still, 0, 0);
    EXPECT_EQ(result.stop, (int)SWITCH_T1V3_TIMING_NOT_PINNED);
    EXPECT_TRUE(still.writes.empty());
    EXPECT_EQ(result.timing_0, kTiming0);
    EXPECT_EQ(result.timing_1, kTiming1);
}

TEST(T1v3Measured, EveryOtherGateAcceptsTheMeasuredWordsAndTheRunReachesTheDivisorWrite)
{
    Still still;
    const auto result = run(still, kTiming0, kTiming1);
    EXPECT_EQ(still.writes.size(), 2u);
    if (still.writes.size() < 2)
        return;
    EXPECT_EQ(still.writes[0].first, (unsigned)T1V3_I2C1_CLK_DIVISOR);
    EXPECT_EQ(still.writes[0].second, SWITCH_T1V3_DIVISOR_AFTER);
    EXPECT_EQ(result.stop, (int)SWITCH_T1V3_DIVISOR_READBACK);
    EXPECT_EQ(still.writes[1].first, (unsigned)T1V3_I2C1_CONFIG_LOAD);
    EXPECT_EQ(still.writes[1].second, SWITCH_T1V3_LOAD_APPLY);
}

TEST(T1v3Measured, AWordThatDiffersFromItsPinStopsBeforeTheFirstWrite)
{
    for (const auto &pins : {std::make_pair(kTiming0 + 1, kTiming1), std::make_pair(kTiming0, kTiming1 + 1)}) {
        Still still;
        const auto result = run(still, pins.first, pins.second);
        EXPECT_EQ(result.stop, (int)SWITCH_T1V3_TIMING_UNEXPECTED);
        EXPECT_TRUE(still.writes.empty());
    }
}
