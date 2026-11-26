// D0-S v2's read logic on the host: the allowlist, the order of the reads and when SDMMC1 is touched.
#include <gtest/gtest.h>

#include <set>
#include <vector>

extern "C" {
#include "../s2.h"
#include "../s2_report.h"
}

#include <cstring>
#include <string>

namespace {
constexpr uint32_t SD = 1u << 14;

struct Fake {
    uint32_t rst_l = 0, enb_l = SD, present = 0x01f70000, word_2c = 0x00000107;
    std::vector<unsigned> reads;
    std::vector<std::string> events; // "show N" and "read N" in the order they happened
    int refuse_step = -1;            // the step whose show returns 0

    static int show(void *context, unsigned step, const switch_s2_result *)
    {
        Fake *self = static_cast<Fake *>(context);
        self->events.push_back("show " + std::to_string(step));
        return static_cast<int>(step) != self->refuse_step;
    }

    static uint32_t read(void *context, unsigned reg)
    {
        Fake *self = static_cast<Fake *>(context);
        self->reads.push_back(reg);
        self->events.push_back("read " + std::to_string(reg));
        switch (reg) {
        case S2_CAR_RST_DEVICES_L: return self->rst_l;
        case S2_CAR_CLK_OUT_ENB_L: return self->enb_l;
        case S2_SDMMC1_PRESENT_STATE: return self->present;
        case S2_SDMMC1_WORD_2C: return self->word_2c;
        }
        return 0xdead;
    }

    switch_s2_result run(bool with_screen = false)
    {
        switch_s2_io io{this, read, with_screen ? show : nullptr};
        switch_s2_result result;
        switch_s2_run(&io, &result);
        return result;
    }
};
}

TEST(S2, ClockOnAndOutOfResetReadsTheCarThenSdmmc1InOrder)
{
    Fake fake;
    const auto result = fake.run();
    EXPECT_EQ(result.stop, 0);
    EXPECT_EQ(result.sd_state, SWITCH_S2_SD_READ);
    EXPECT_EQ(fake.reads, (std::vector<unsigned>{S2_CAR_RST_DEVICES_L, S2_CAR_CLK_OUT_ENB_L, S2_SDMMC1_PRESENT_STATE, S2_SDMMC1_WORD_2C}));
    EXPECT_EQ(result.reads, 4u);
    EXPECT_EQ(result.sd_present_state, 0x01f70000u);
    EXPECT_EQ(result.sd_word_2c, 0x107u);
}

TEST(S2, ClockOffNeverTouchesSdmmc1)
{
    Fake fake;
    fake.enb_l = 0;
    const auto result = fake.run();
    EXPECT_EQ(result.sd_state, SWITCH_S2_SD_CLOCK_OFF);
    EXPECT_EQ(fake.reads.size(), 2u);
}

TEST(S2, InResetNeverTouchesSdmmc1)
{
    Fake fake;
    fake.rst_l = SD;
    const auto result = fake.run();
    EXPECT_EQ(result.sd_state, SWITCH_S2_SD_IN_RESET);
    EXPECT_EQ(fake.reads.size(), 2u);
}

TEST(S2, OtherBitsDoNotDecideTheSdmmc1Gate)
{
    Fake fake;
    fake.enb_l = ~SD;
    EXPECT_EQ(fake.run().sd_state, SWITCH_S2_SD_CLOCK_OFF);
    Fake other;
    other.rst_l = ~SD;
    EXPECT_EQ(other.run().sd_state, SWITCH_S2_SD_READ);
}

TEST(S2, TheWordsHekateLeftFromTheT1tRunOpenTheSdmmc1Gate)
{
    Fake fake;
    fake.rst_l = 0x049102ca;
    fake.enb_l = 0x9802d131;
    const auto result = fake.run();
    EXPECT_EQ(result.sd_state, SWITCH_S2_SD_READ);
    EXPECT_EQ(result.reads, 4u);
}

TEST(S2, AllowlistIsTheFourAddressesAndNothingElse)
{
    std::set<uint32_t> addresses;
    for (unsigned reg = 0; reg < 64; ++reg) {
        const bool allowed = switch_s2_may_read(reg);
        EXPECT_EQ(allowed, reg < S2_REG_COUNT) << reg;
        if (allowed) {
            addresses.insert(switch_s2_address(reg));
            EXPECT_EQ(switch_s2_address(reg) & 3u, 0u);
        } else {
            EXPECT_EQ(switch_s2_address(reg), 0u);
        }
    }
    EXPECT_EQ(addresses, (std::set<uint32_t>{0x60006004u, 0x60006010u, 0x700b0024u, 0x700b002cu}));
}

TEST(S2, ResultStartsCleared)
{
    switch_s2_result result;
    for (auto *p = reinterpret_cast<volatile unsigned char *>(&result); p < reinterpret_cast<unsigned char *>(&result + 1); ++p)
        *p = 0xa5;
    Fake fake;
    switch_s2_io io{&fake, Fake::read, nullptr};
    switch_s2_run(&io, &result);
    EXPECT_EQ(result.stop, 0);
}

namespace {
std::string line_of(unsigned step, const switch_s2_result &r)
{
    switch_s2_line line;
    switch_s2_screen_line(step, &r, &line);
    return std::string(line.text.text, line.text.length);
}

std::vector<std::string> expected_events(bool sd)
{
    std::vector<std::string> e = {"show 0", "show 1", "read 0", "show 2", "show 3", "read 1", "show 4", "show 5"};
    if (sd) {
        for (const char *s : {"show 6", "read 2", "show 7", "show 8", "read 3", "show 9"})
            e.push_back(s);
    }
    e.push_back("show 10");
    return e;
}
}

TEST(S2, EveryReadIsBetweenItsBeginAndResultSteps)
{
    Fake fake;
    const auto result = fake.run(true);
    EXPECT_EQ(result.stop, 0);
    EXPECT_EQ(fake.events, expected_events(true));
}

TEST(S2, AClosedGateShowsTheGateAndCompleteAndReadsNothingMore)
{
    Fake fake;
    fake.enb_l = 0;
    fake.run(true);
    EXPECT_EQ(fake.events, expected_events(false));
}

TEST(S2, AStepThatDoesNotSettleStopsTheRunBeforeTheNextRead)
{
    for (int step = 0; step < S2_STEP_COUNT - 1; ++step) {
        Fake fake;
        fake.refuse_step = step;
        const auto result = fake.run(true);
        EXPECT_EQ(result.stop, SWITCH_S2_NOT_SHOWN) << step;
        // A read is never made once a line before it failed: reads so far never exceed those whose BEGIN settled.
        const unsigned began = step < S2_STEP_CAR_RST_BEGIN ? 0 : step < S2_STEP_CAR_CLK_BEGIN ? 1 : step < S2_STEP_PRESENT_BEGIN ? 2
                               : step < S2_STEP_CLOCK_WORD_BEGIN ? 2 : 3;
        EXPECT_LE(fake.reads.size(), began + 1u) << step;
        EXPECT_EQ(fake.events.back(), "show 10") << step;
    }
}

TEST(S2, ABeginLineThatDoesNotSettleMeansThatReadIsNotMade)
{
    Fake fake;
    fake.refuse_step = S2_STEP_CAR_CLK_BEGIN;
    fake.run(true);
    EXPECT_EQ(fake.reads, (std::vector<unsigned>{S2_CAR_RST_DEVICES_L}));
}

TEST(S2, TheScreenLinesNameEachStepAndCarryTheWordRead)
{
    switch_s2_result r{};
    r.rst_devices_l = 0x049102ca;
    r.clk_out_enb_l = 0x9802d131;
    r.sd_present_state = 0x01f70000;
    r.sd_word_2c = 0x107;
    r.reads = 4;
    EXPECT_EQ(line_of(S2_STEP_START, r), "S2 START. EL1 STATE PASS");
    EXPECT_EQ(line_of(S2_STEP_CAR_RST_BEGIN, r), "S2 CAR RST READ BEGIN. ADDRESS 60006004");
    EXPECT_EQ(line_of(S2_STEP_CAR_RST_RESULT, r), "S2 CAR RST READ RESULT 049102CA");
    EXPECT_EQ(line_of(S2_STEP_CAR_CLK_BEGIN, r), "S2 CAR CLK READ BEGIN. ADDRESS 60006010");
    EXPECT_EQ(line_of(S2_STEP_CAR_CLK_RESULT, r), "S2 CAR CLK READ RESULT 9802D131");
    EXPECT_EQ(line_of(S2_STEP_GATE, r), "S2 SDMMC1 CLOCK/RESET GATE. CLOCK ON. RESET OFF. READ");
    EXPECT_EQ(line_of(S2_STEP_PRESENT_BEGIN, r), "S2 SDMMC1 PRESENT STATE READ BEGIN. ADDRESS 700B0024");
    EXPECT_EQ(line_of(S2_STEP_PRESENT_RESULT, r), "S2 SDMMC1 PRESENT STATE READ RESULT 01F70000");
    EXPECT_EQ(line_of(S2_STEP_CLOCK_WORD_BEGIN, r), "S2 SDMMC1 CLOCK WORD READ BEGIN. ADDRESS 700B002C");
    EXPECT_EQ(line_of(S2_STEP_CLOCK_WORD_RESULT, r), "S2 SDMMC1 CLOCK WORD READ RESULT 00000107");
    EXPECT_EQ(line_of(S2_STEP_COMPLETE, r), "S2 COMPLETE. READS 4. STOP 0000");
    r.sd_state = SWITCH_S2_SD_CLOCK_OFF;
    EXPECT_EQ(line_of(S2_STEP_GATE, r), "S2 SDMMC1 CLOCK/RESET GATE. CLOCK OFF. NOT READ");
    r.sd_state = SWITCH_S2_SD_IN_RESET;
    EXPECT_EQ(line_of(S2_STEP_GATE, r), "S2 SDMMC1 CLOCK/RESET GATE. IN RESET. NOT READ");
}

TEST(S2, ALineThatReportsAStopIsDrawnAsBad)
{
    switch_s2_result r{};
    r.stop = SWITCH_S2_NOT_SHOWN;
    switch_s2_line line;
    switch_s2_screen_line(S2_STEP_COMPLETE, &r, &line);
    EXPECT_TRUE(line.bad);
    EXPECT_EQ(std::string(line.text.text, line.text.length), "S2 COMPLETE. READS 0. STOP 0802");
}

TEST(S2, EveryLineUsesOnlyCharactersTheFontHas)
{
    const std::string font = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-.=/()_? ";
    switch_s2_result r{};
    r.reads = 4;
    r.stop = SWITCH_S2_NOT_SHOWN;
    for (unsigned state : {SWITCH_S2_SD_READ, SWITCH_S2_SD_CLOCK_OFF, SWITCH_S2_SD_IN_RESET})
        for (unsigned step = 0; step < S2_STEP_COUNT; ++step) {
            r.sd_state = state;
            for (char c : line_of(step, r))
                EXPECT_NE(font.find(c), std::string::npos) << step << " " << c;
        }
}

TEST(S2, TheSettleLastsOneHundredMillisecondsOnlyAtTheExpectedFrequency)
{
    EXPECT_EQ(switch_s2_settle_ticks(19200000), 1920000u);
    EXPECT_EQ(switch_s2_settle_ticks(0), 0u);
    EXPECT_EQ(switch_s2_settle_ticks(24000000), 0u);
    EXPECT_GE(SWITCH_S2_SETTLE_MS, 17u * 2); // two frames at 60 Hz, at least
}
