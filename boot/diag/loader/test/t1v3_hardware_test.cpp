// T1 v3 against the words and times its first hardware run reported (boot/diag/erista-t1v3-run1.txt). The model replays
// that run: it is the code's logic that is under test, not the hardware. The screen lines are the photographed text.
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "../t1v3.h"
#include "../t1v3_report.h"
}

extern "C" {
uint32_t switch_t1v3_read32(uint32_t)
{
    return 0;
}
void switch_t1v3_write32(uint32_t, uint32_t) {}
}

namespace {
using Write = std::pair<unsigned, uint32_t>;

struct Replay {
    std::vector<Write> writes;
    uint64_t clock = 0, go_at = 0;
    bool in_transfer = false;
    unsigned goes = 0;
    uint32_t divisor = 0x00050001u, cnfg = 0x00002842u, data1 = 0;
    const unsigned busy_at[2] = {6, 5};
    /* The poll that sees BUSY clear is followed by one counter read that advances the model's clock by 1. */
    const unsigned busy_clear = 432;

    uint32_t status()
    {
        if (!in_transfer)
            return 0;
        clock += 1;
        const unsigned at = busy_at[goes - 1];
        if (clock - go_at >= busy_clear) {
            in_transfer = false;
            return 0;
        }
        return clock - go_at >= at ? 0x100u : 0;
    }

    uint32_t read(unsigned reg)
    {
        switch (reg) {
        case T1V3_CAR_RST_DEVICES_L: return 0x049102CA;
        case T1V3_CAR_CLK_OUT_ENB_L: return 0x9802D131;
        case T1V3_CAR_CLK_SOURCE_I2C1: return 0x00000003;
        case T1V3_CAR_PLLP_BASE: return 0x48115408;
        case T1V3_I2C1_CNFG: return cnfg;
        case T1V3_I2C1_STATUS: return status();
        case T1V3_I2C1_CLK_DIVISOR: return divisor;
        case T1V3_I2C1_CONFIG_LOAD: return 0x00000004;
        case T1V3_I2C1_TIMING_0: return 0x00000204;
        case T1V3_I2C1_TIMING_1: return 0x04070404;
        case T1V3_I2C1_CMD_DATA1: return data1;
        }
        return 0;
    }

    void write(unsigned reg, uint32_t value)
    {
        writes.emplace_back(reg, value);
        if (reg == T1V3_I2C1_CLK_DIVISOR)
            divisor = value;
        if (reg == T1V3_I2C1_CMD_DATA1)
            data1 = value;
        if (reg == T1V3_I2C1_CNFG && (value & 0x200)) {
            go_at = clock;
            in_transfer = true;
            ++goes;
            cnfg = value & ~0x200u;
            if (goes == 2)
                data1 = 0x55;
        } else if (reg == T1V3_I2C1_CNFG) {
            cnfg = value;
        }
    }

    uint64_t now()
    {
        if (!in_transfer)
            clock += 1;
        return clock;
    }
};

uint32_t read_replay(void *c, unsigned reg) { return static_cast<Replay *>(c)->read(reg); }
void write_replay(void *c, unsigned reg, uint32_t value) { static_cast<Replay *>(c)->write(reg, value); }
uint64_t now_replay(void *c) { return static_cast<Replay *>(c)->now(); }

switch_t1v3_result run(Replay &replay)
{
    const switch_t1v3_io io = {&replay, SWITCH_T1V3_TIMING_0_PIN, SWITCH_T1V3_TIMING_1_PIN, read_replay, write_replay,
                               now_replay};
    switch_t1v3_result result;
    switch_t1v3_run(&io, &result);
    return result;
}

std::vector<std::string> screen(const switch_t1v3_result &r)
{
    switch_t1v3_line lines[SWITCH_T1V3_LINES];
    switch_t1v3_lines(&r, lines);
    std::vector<std::string> out;
    for (const auto &line : lines)
        out.push_back(line.text.text);
    return out;
}
} // namespace

TEST(T1v3Hardware, TheReplayedRunEndsWithTheReportedWords)
{
    Replay replay;
    const auto r = run(replay);
    EXPECT_EQ(r.stop, 0);
    EXPECT_EQ(r.writes, 11u);
    EXPECT_EQ(r.transfers, 2u);
    EXPECT_EQ(r.divisor, 0x00050001u);
    EXPECT_EQ(r.divisor_after, 0x00ff0001u);
    EXPECT_EQ(r.load_after, 4u);
    EXPECT_EQ(r.load_transfer[0], 4u);
    EXPECT_EQ(r.load_transfer[1], 4u);
    EXPECT_EQ(r.data_before_read, 0xfeu);
    EXPECT_EQ(r.id, 0x55);
    EXPECT_TRUE(r.have_id);
    EXPECT_EQ(r.transaction[0].started_us, 6u);
    EXPECT_EQ(r.transaction[0].elapsed_us, 433u);
    EXPECT_EQ(r.transaction[1].started_us, 5u);
    EXPECT_EQ(r.transaction[1].elapsed_us, 433u);
    EXPECT_EQ(r.transaction[0].cnfg, 0x00002800u);
    EXPECT_EQ(r.transaction[1].cnfg, 0x00002840u);
    EXPECT_EQ(r.status_after, 0u);
    EXPECT_EQ(r.int_status_after, 0u);
}

TEST(T1v3Hardware, TheScreenLinesAreThePhotographedText)
{
    Replay replay;
    const auto lines = screen(run(replay));
    ASSERT_EQ(lines.size(), (size_t)SWITCH_T1V3_LINES);
    EXPECT_EQ(lines[0], "CAR RST_L 049102CA ENB_L 9802D131 SRC_I2C1 00000003 (EXPECT 00000003)");
    EXPECT_EQ(lines[1], "PLLP_BASE 48115408 M 8 N 85 P 1 (EXPECT 8 85 1). MODEL ONLY, NOT A MEASURED RATE");
    EXPECT_EQ(lines[2], "BEFORE CNFG 00002842 STATUS 00000000 INT 00000000 DIV 00050001 LOAD 00000004");
    EXPECT_EQ(lines[3], "TIMING_0 00000204 TIMING_1 04070404. READ ONLY. 061A: NO REVIEWED VALUE");
    EXPECT_EQ(lines[4], "AFTER LOAD 00000004 DIV 00FF0001 STATUS 00000000 INT 00000000. DIV WRITTEN 00FF0001, NOT RESTORED");
    EXPECT_EQ(lines[5], "LOAD AFTER 0X25 00000004 AND 00000004 (EXPECT 00000004 BEFORE EACH GO)");
    EXPECT_EQ(lines[6], "WRITE POINTER BUSY AT 6 US, CLEAR AT 433 US. AFTER GO CNFG 00002800 INT 00000000");
    EXPECT_EQ(lines[7], "READ BUSY AT 5 US, CLEAR AT 433 US. AFTER GO CNFG 00002840 INT 00000000");
    EXPECT_EQ(lines[9], "TMP451 0X4C ID(FE) 55 EXPECT 55. DATA1 BEFORE READ FE. COMMUNICATION CHECK ONLY");
    EXPECT_EQ(lines[10], "T1V3 COMPLETE CODE 0000. COMPLETE");
    EXPECT_EQ(lines[11], "WRITES 11. TRANSFERS 2 OF 2. NO RETRY. NO CAR WRITE. STATUS AFTER 00000000 INT 00000000");
}

TEST(T1v3Hardware, TheReplayWritesOnlyTheElevenReviewedWordsToI2c1)
{
    Replay replay;
    run(replay);
    const std::vector<Write> expected = {
        {T1V3_I2C1_CLK_DIVISOR, 0x00ff0001}, {T1V3_I2C1_CONFIG_LOAD, 0x01}, {T1V3_I2C1_CMD_ADDR0, 0x98},
        {T1V3_I2C1_CMD_DATA1, 0xfe},         {T1V3_I2C1_CNFG, 0x2800},      {T1V3_I2C1_CONFIG_LOAD, 0x25},
        {T1V3_I2C1_CNFG, 0x2a00},            {T1V3_I2C1_CMD_ADDR0, 0x99},   {T1V3_I2C1_CNFG, 0x2840},
        {T1V3_I2C1_CONFIG_LOAD, 0x25},       {T1V3_I2C1_CNFG, 0x2a40},
    };
    EXPECT_EQ(replay.writes, expected);
}

TEST(T1v3Hardware, ALoadWordThatKeepsBit2AfterTheApplyIsTheMeasuredCaseAndStopsNothing)
{
    Replay replay;
    const auto r = run(replay);
    EXPECT_EQ(r.load_after & 4u, 4u);
    EXPECT_EQ(r.stop, 0);
}
