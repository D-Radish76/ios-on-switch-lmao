// T1-T first hardware run (boot/diag/erista-t1t-run1.txt) as a fixture: the run reproduces the screen that was
// photographed, and T1 v3's read-phase gates accept the I2C1 words once the two timing words are pinned.
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

extern "C" {
#include "../t1t.h"
#include "../t1t_report.h"
}

extern "C" uint32_t switch_t1t_read32(uint32_t)
{
    return 0;
}

namespace {
struct Word {
    const char *name;
    uint32_t value;
};

// Keep this table equal to boot/diag/erista-t1t-run1.txt (diag_t1t_measured_test.py compares them).
const Word measured[] = {
    {"RST_L", 0x049102CA},
    {"ENB_L", 0x9802D131},
    {"RST_H", 0x84D07326},
    {"ENB_H", 0x030180C1},
    {"SRC_I2C1", 0x00000003},
    {"SRC_I2C2", 0x00000000},
    {"SRC_I2C3", 0x00000000},
    {"SRC_I2C4", 0x00000000},
    {"SRC_I2C5", 0x00000003},
    {"SRC_I2C6", 0x00000000},
    {"PLLP_BASE", 0x48115408},
    {"I2C1_CNFG", 0x00002842},
    {"I2C1_STATUS", 0x00000000},
    {"I2C1_INT_STATUS", 0x00000000},
    {"I2C1_CLK_DIVISOR", 0x00050001},
    {"I2C1_CONFIG_LOAD", 0x00000004},
    {"I2C1_TIMING_0", 0x00000204},
    {"I2C1_TIMING_1", 0x04070404},
    {"I2C5_CNFG", 0x00000C00},
    {"I2C5_STATUS", 0x00000100},
    {"I2C5_INT_STATUS", 0x000000C2},
    {"I2C5_CLK_DIVISOR", 0x00190001},
    {"I2C5_CONFIG_LOAD", 0x00000000},
    {"I2C5_TIMING_0", 0x00000204},
    {"I2C5_TIMING_1", 0x04070404},
};

uint32_t word(const std::string &name)
{
    for (const Word &w : measured)
        if (name == w.name)
            return w.value;
    EXPECT_TRUE(name.empty());
    return 0;
}

struct Board {
    std::map<unsigned, uint32_t> reg;
    Board()
    {
        reg[T1T_CAR_RST_DEVICES_L] = word("RST_L");
        reg[T1T_CAR_CLK_OUT_ENB_L] = word("ENB_L");
        reg[T1T_CAR_RST_DEVICES_H] = word("RST_H");
        reg[T1T_CAR_CLK_OUT_ENB_H] = word("ENB_H");
        for (unsigned bus = 0; bus < SWITCH_T1T_BUSES; ++bus)
            reg[T1T_CAR_SOURCE_I2C1 + bus] = word("SRC_I2C" + std::to_string(bus + 1));
        reg[T1T_CAR_PLLP_BASE] = word("PLLP_BASE");
        const char *names[T1T_BLOCK_WORDS] = {"CNFG", "STATUS", "INT_STATUS", "CLK_DIVISOR", "CONFIG_LOAD", "TIMING_0", "TIMING_1"};
        for (unsigned block = 0; block < SWITCH_T1T_BLOCKS; ++block)
            for (unsigned w = 0; w < T1T_BLOCK_WORDS; ++w)
                reg[SWITCH_T1T_I2C_REG(block, w)] = word(std::string(block ? "I2C5_" : "I2C1_") + names[w]);
    }
};

uint32_t read_board(void *context, unsigned reg)
{
    return static_cast<Board *>(context)->reg.at(reg);
}

switch_t1t_result run_measured()
{
    Board board;
    switch_t1t_io io = {&board, read_board};
    switch_t1t_result result;
    switch_t1t_run(&io, &result);
    return result;
}

std::vector<std::string> screen()
{
    const switch_t1t_result result = run_measured();
    switch_t1t_line lines[SWITCH_T1T_LINES];
    switch_t1t_lines(&result, lines);
    std::vector<std::string> out;
    for (const auto &line : lines)
        out.emplace_back(line.text.text, line.text.length);
    return out;
}
} // namespace

TEST(T1tMeasured, TheRunReadsTwentyFiveWordsAndCompletes)
{
    const auto result = run_measured();
    EXPECT_EQ(result.stop, 0);
    EXPECT_EQ(result.reads, 25u);
    EXPECT_EQ(result.block[0].state, SWITCH_T1T_BLOCK_READ);
    EXPECT_EQ(result.block[1].state, SWITCH_T1T_BLOCK_READ);
}

TEST(T1tMeasured, BothControllersHaveTheirClockOnAndAreOutOfReset)
{
    EXPECT_TRUE(word("ENB_L") & (1u << 12));
    EXPECT_FALSE(word("RST_L") & (1u << 12));
    EXPECT_TRUE(word("ENB_H") & (1u << 15));
    EXPECT_FALSE(word("RST_H") & (1u << 15));
}

TEST(T1tMeasured, TheScreenIsTheOneThatWasPhotographed)
{
    const std::vector<std::string> expected = {
        "T1T LOADER AT EL1. CAR RST_L 049102CA ENB_L 9802D131 RST_H 84D07326 ENB_H 030180C1",
        "I2C1 CLOCK ON OUT OF RESET. I2C5 CLOCK ON OUT OF RESET. READ ONLY",
        "SRC I2C1 00000003 I2C2 00000000 I2C3 00000000",
        "SRC I2C4 00000000 I2C5 00000003 I2C6 00000000. FIELD 31:30 0 PLL_P 3 CLK_M (LINUX, NOT THE TRM)",
        "PLLP_BASE 48115408 M 8 N 85 P 1. RAW WORD AND FIELDS. NO FREQUENCY CLAIMED",
        "I2C1 CNFG 00002842 STATUS 00000000 BUSY 0 NOACK 0 INT 00000000",
        "I2C1 DIV 000500010005 STD/FAST FIELD, HS 0001. LOAD 00000004 BIT0 0 BIT2 1 BIT5 0 OTHER 00000000",
        "I2C1 TIMING_0 00000204 TLOW 4 THIGH 2 OTHER BITS 00000000",
        "I2C1 TIMING_1 04070404 TSU_STA 4 THD_STA 4 TSU_STO 7 TBUF 4 OTHER BITS 00000000",
        "I2C5 CNFG 00000C00 STATUS 00000100 BUSY 1 NOACK 0 INT 000000C2",
        "I2C5 DIV 001900010019 STD/FAST FIELD, HS 0001. LOAD 00000000 BIT0 0 BIT2 0 BIT5 0 OTHER 00000000",
        "I2C5 TIMING_0 00000204 TLOW 4 THIGH 2 OTHER BITS 00000000",
        "I2C5 TIMING_1 04070404 TSU_STA 4 THD_STA 4 TSU_STO 7 TBUF 4 OTHER BITS 00000000",
        "TIMING_0 EQUAL ON BOTH. TIMING_1 EQUAL ON BOTH. EQUAL WORDS ARE NOT PROOF OF A RESET VALUE",
        "T1T COMPLETE CODE 0000. READ ONLY. NO I2C TRANSFER. NO REGISTER WRITE",
        "REGISTER READS 25. VALUES ARE CANDIDATES FOR REVIEW, NOT A SAFETY PROOF",
    };
    EXPECT_EQ(screen(), expected);
}

TEST(T1tMeasured, TheTimingFieldsAreTheOnesTheScreenShows)
{
    for (const char *bus : {"I2C1_", "I2C5_"}) {
        const uint32_t t0 = word(std::string(bus) + "TIMING_0"), t1 = word(std::string(bus) + "TIMING_1");
        EXPECT_EQ(SWITCH_T1T_TLOW(t0), 4u);
        EXPECT_EQ(SWITCH_T1T_THIGH(t0), 2u);
        EXPECT_EQ(t0 & ~SWITCH_T1T_TIMING_0_FIELDS, 0u);
        EXPECT_EQ(SWITCH_T1T_TSU_STA(t1), 4u);
        EXPECT_EQ(SWITCH_T1T_THD_STA(t1), 4u);
        EXPECT_EQ(SWITCH_T1T_TSU_STO(t1), 7u);
        EXPECT_EQ(SWITCH_T1T_TBUF(t1), 4u);
        EXPECT_EQ(t1 & ~SWITCH_T1T_TIMING_1_FIELDS, 0u);
    }
}

TEST(T1tMeasured, PllpBaseIsTheRowLinuxUsesForA408MhzOutput)
{
    const uint32_t base = word("PLLP_BASE");
    EXPECT_EQ(SWITCH_T1T_PLLP_M(base), 8u);
    EXPECT_EQ(SWITCH_T1T_PLLP_N(base), 85u);
    EXPECT_EQ(SWITCH_T1T_PLLP_P(base), 1u);
}

TEST(T1tMeasured, OnlyI2c1AndI2c5HaveTheSourceWordThree)
{
    for (const char *bus : {"SRC_I2C1", "SRC_I2C5"})
        EXPECT_EQ(word(bus), 3u);
    for (const char *bus : {"SRC_I2C2", "SRC_I2C3", "SRC_I2C4", "SRC_I2C6"})
        EXPECT_EQ(word(bus), 0u);
}
