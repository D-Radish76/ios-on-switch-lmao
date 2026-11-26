// The T2 image against the words the first hardware run measured (boot/diag/erista-t2-run1.txt): the same sensor bytes, the same controller words,
// a conversion that ended before the 31 ms poll. The model's own times are not the hardware's; only the counts and the text are compared.
#include <gtest/gtest.h>

#include <string>

#include "t2_model.h"

using namespace t2model;

namespace {
void measure(Model &m, uint64_t conversion_us)
{
    m.car_rst = 0x049102CA;
    m.car_enb = 0x9802D131;
    m.car_src = 0x00000003;
    m.car_pllp = 0x48115408;
    m.cnfg = 0x00002842;
    m.sensor = {{0xfe, 0x55}, {0x03, 0xc0}, {0x11, 0x00}, {0x12, 0x00}, {0x02, 0x00},
                {0x01, 0x22}, {0x10, 0x10}, {0x00, 0x1f}, {0x15, 0xd0}};
    m.after_conversion = {{0x01, 0x22}, {0x10, 0x50}, {0x00, 0x1f}, {0x15, 0xf0}};
    m.conversion_us = conversion_us;
}

std::string text(const switch_t2_line &line)
{
    return line.text.text;
}
}  // namespace

TEST(T2Measured, AConversionThatEndsBeforeThe31MsPollGivesTheScreensCounts)
{
    for (uint64_t conversion_us : {10000u, 20000u, 25000u, 30000u}) {
        Rig r;
        measure(r.model, conversion_us);
        switch_t2_image_result result;
        switch_t2_run_with(&r.io, &result);
        ASSERT_EQ(result.stop, 0) << conversion_us;
        EXPECT_EQ(result.writes, 160u) << conversion_us;
        EXPECT_EQ(result.transfers, 35u) << conversion_us;
        EXPECT_EQ(r.model.writes.size(), 160u) << conversion_us;
        EXPECT_EQ(r.model.sensor_writes.size(), 1u) << conversion_us;
    }
}

TEST(T2Measured, ALongerConversionNeedsAThirdPollAndNineMoreWrites)
{
    Rig r;
    measure(r.model, 34000);
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    ASSERT_EQ(result.stop, 0);
    EXPECT_EQ(result.writes, 169u);
    EXPECT_EQ(result.transfers, 37u);
}

TEST(T2Measured, TheScreenTextOfTheRun)
{
    Rig r;
    measure(r.model, 25000);
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    switch_t2_line lines[SWITCH_T2_LINES];
    switch_t2_lines(&result, lines);
    EXPECT_EQ("CAR RST_L 049102CA ENB_L 9802D131 SRC_I2C1 00000003 PLLP_BASE 48115408", text(lines[0]));
    EXPECT_EQ("BEFORE CNFG 00002842 STATUS 00000000 INT 00000000 DIV 00050001 LOAD 00000004", text(lines[1]));
    EXPECT_EQ("TIMING_0 00000204 TIMING_1 04070404", text(lines[2]));
    EXPECT_EQ("AFTER LOAD 00000004 DIV 00FF0001 STATUS 00000000 INT 00000000. DIV WRITTEN 00FF0001, NOT RESTORED", text(lines[3]));
    EXPECT_EQ("TMP451 0X4C ID(FE) 55 CONFIG(03) C0 STATUS(02) 00 OFFSET(11,12) 00.00", text(lines[4]));
    EXPECT_EQ("BEFORE THE ONE-SHOT REMOTE 22.10 LOCAL 1F.D0", text(lines[5]));
    EXPECT_EQ(0u, text(lines[6]).find("ONE-SHOT WRITTEN. BUSY SEEN AT "));
    EXPECT_NE(std::string::npos, text(lines[6]).find(" US (MIN 31000, BOUND 100000). LAST STATUS 00"));
    EXPECT_EQ("AFTER REMOTE 22.50 LOCAL 1F.F0. CONFIG C0 BACK IN SHUTDOWN", text(lines[7]));
    EXPECT_EQ("REMOTE 34.3125 C, LOCAL 31.9375 C. ONE READING. NOT A LIMIT AND NOT A SAFETY CRITERION", text(lines[8]));
    EXPECT_EQ("T2 COMPLETE CODE 0000. COMPLETE", text(lines[12]));
    EXPECT_EQ("WRITES 160. TRANSFERS 35. NO RETRY. NO CAR WRITE. ONE SENSOR WRITE AT MOST", text(lines[13]));
    for (unsigned i = 0; i < SWITCH_T2_LINES; ++i)
        EXPECT_EQ(0, lines[i].bad) << i;
}

TEST(T2Measured, TheWritesAreTheFiveI2c1RegistersAndTheBusWasLeftAtTheNewDivisor)
{
    Rig r;
    measure(r.model, 25000);
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    const std::set<unsigned> allowed = {T2_HW_I2C1_CLK_DIVISOR, T2_HW_I2C1_CNFG, T2_HW_I2C1_CMD_ADDR0, T2_HW_I2C1_CMD_DATA1,
                                        T2_HW_I2C1_CONFIG_LOAD};
    EXPECT_EQ(allowed, r.model.regs_written);
    EXPECT_EQ(0x00ff0001u, r.model.divisor);
    EXPECT_EQ(0xc0, r.model.sensor[0x03]);
}
