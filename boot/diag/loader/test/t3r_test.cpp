// T3-R's read logic on the host: the allowlist, the order of the reads, the PWM gate and classification, the stop
// conditions, the read budget and the screen lines.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../t3r.h"
#include "../t3r_report.h"
}

namespace {
struct Fake {
    uint32_t value[T3R_REG_COUNT] = {};
    uint32_t tmrus_second = 1200000;
    std::vector<std::string> events; // "show N" and "read N" in the order they happened
    std::map<unsigned, switch_t3r_result> drawn; // the result structure as each step was shown
    int refuse_step = -1;
    int expire_at_check = -1; // the nth call of expired() and every later one answer yes
    int checks = 0;
    unsigned tmrus_reads = 0;

    Fake()
    {
        value[T3R_TMRUS] = 1000000;
        value[T3R_CAR_RST_DEVICES_L] = 0x049102CA;
        value[T3R_CAR_CLK_OUT_ENB_L] = 0x9802D131;
        value[T3R_PWM_CSR_1] = SWITCH_T3R_PWM_OFF_WORD;
        value[T3R_PINMUX_LCD_GPIO2] = 0x74;
        value[T3R_PINMUX_CAM1_PWDN] = 0x59;
        value[T3R_GPIO_S_CNF] = 0x80;
    }

    static int show(void *context, unsigned step, const switch_t3r_result *result)
    {
        Fake *self = static_cast<Fake *>(context);
        self->events.push_back("show " + std::to_string(step));
        self->drawn[step] = *result;
        return static_cast<int>(step) != self->refuse_step;
    }

    static uint32_t read(void *context, unsigned reg)
    {
        Fake *self = static_cast<Fake *>(context);
        self->events.push_back("read " + std::to_string(reg));
        if (reg == T3R_TMRUS && self->tmrus_reads++)
            return self->tmrus_second;
        return self->value[reg];
    }

    static int expired(void *context)
    {
        Fake *self = static_cast<Fake *>(context);
        return self->expire_at_check >= 0 && self->checks++ >= self->expire_at_check;
    }

    switch_t3r_result run()
    {
        switch_t3r_io io{this, read, show, expired};
        switch_t3r_result result;
        switch_t3r_run(&io, &result);
        return result;
    }

    std::vector<unsigned> read_regs() const
    {
        std::vector<unsigned> regs;
        for (const auto &e : events)
            if (e.rfind("read", 0) == 0)
                regs.push_back(static_cast<unsigned>(std::stoi(e.substr(5))));
        return regs;
    }
};

std::string text_of(unsigned step, const switch_t3r_result &result, switch_t3r_line *line = nullptr)
{
    switch_t3r_line local;
    switch_t3r_line *out = line ? line : &local;
    switch_t3r_screen_line(step, &result, out);
    return out->text.text;
}
} // namespace

TEST(T3R, TwentyTwoReadsInTheDesignedOrderEachBetweenItsBeginAndResult)
{
    Fake fake;
    const switch_t3r_result result = fake.run();
    EXPECT_EQ(0, result.stop);
    EXPECT_EQ(22u, result.reads);
    const std::vector<unsigned> regs = fake.read_regs();
    ASSERT_EQ(22u, regs.size());
    for (unsigned i = 0; i < 22; ++i)
        EXPECT_EQ(switch_t3r_sequence(i), regs[i]) << i;
    EXPECT_EQ(T3R_TMRUS, regs.front());
    EXPECT_EQ(T3R_TMRUS, regs.back());
    EXPECT_EQ(T3R_CAR_RST_DEVICES_L, regs[1]);
    EXPECT_EQ(T3R_CAR_CLK_OUT_ENB_L, regs[2]);
    EXPECT_EQ(T3R_PWM_CSR_1, regs[3]);
    EXPECT_EQ(T3R_PMC_NO_IOPOWER, regs[19]);
    EXPECT_EQ(T3R_PMC_PWR_DET_VAL, regs[20]);
    for (size_t i = 0; i < fake.events.size(); ++i) {
        if (fake.events[i].rfind("read", 0) != 0)
            continue;
        EXPECT_EQ(0u, fake.events[i - 1].rfind("show", 0)) << fake.events[i];
        EXPECT_EQ(0u, fake.events[i + 1].rfind("show", 0)) << fake.events[i];
    }
    EXPECT_TRUE(result.tmrus_advanced);
    EXPECT_EQ(static_cast<unsigned>(T3R_GATE_OPEN), result.gate);
    EXPECT_EQ(static_cast<unsigned>(T3R_PWM_OFF_HEKATE), result.pwm);
}

TEST(T3R, EachBeginPrecedesItsReadAndEachResultFollowsIt)
{
    Fake fake;
    fake.run();
    for (unsigned i = 0; i < 22; ++i) {
        const std::string begin = "show " + std::to_string(SWITCH_T3R_STEP_BEGIN(i));
        const std::string result = "show " + std::to_string(SWITCH_T3R_STEP_RESULT(i));
        size_t b = 0, r = 0;
        for (size_t k = 0; k < fake.events.size(); ++k) {
            b = fake.events[k] == begin ? k : b;
            r = fake.events[k] == result ? k : r;
        }
        ASSERT_GT(b, 0u);
        EXPECT_EQ(0u, fake.events[b + 1].rfind("read", 0)) << i;
        EXPECT_EQ(b + 2, r) << i;
    }
}

TEST(T3R, AShownStepThatDoesNotSettleStopsBeforeTheNextRead)
{
    for (unsigned step = 0; step < T3R_STEP_COUNT; ++step) {
        if (step == T3R_STEP_SKIP_PWM)
            continue; // not shown while the gate is open
        Fake fake;
        fake.refuse_step = static_cast<int>(step);
        const switch_t3r_result result = fake.run();
        EXPECT_EQ(static_cast<int>(SWITCH_T3R_NOT_SHOWN), result.stop) << step;
        const std::string refused = "show " + std::to_string(step);
        size_t at = 0;
        for (size_t k = 0; k < fake.events.size(); ++k)
            if (fake.events[k] == refused)
                at = k;
        for (size_t k = at + 1; k < fake.events.size(); ++k)
            if (step != T3R_STEP_COMPLETE)
                EXPECT_NE(0u, fake.events[k].rfind("read", 0)) << step << " " << fake.events[k];
    }
}

TEST(T3R, AllowlistIsTwentyOneWordAddressesAtThirtyTwoBitsAndNothingElse)
{
    std::set<uint32_t> allowed;
    for (unsigned reg = 0; reg < 64; ++reg)
        for (unsigned width = 0; width <= 64; ++width)
            if (switch_t3r_may_read(reg, width)) {
                EXPECT_EQ(32u, width);
                EXPECT_LT(reg, static_cast<unsigned>(T3R_REG_COUNT));
                allowed.insert(switch_t3r_address(reg));
            }
    const std::set<uint32_t> expected = {
        0x60005010, 0x60006004, 0x60006010, 0x7000A010, 0x7000320C, 0x700031EC, 0x7000304C,
        0x700031A8, 0x6000D000, 0x6000D010, 0x6000D020, 0x6000D700, 0x6000D710, 0x6000D720,
        0x6000D408, 0x6000D418, 0x6000D438, 0x6000D504, 0x6000D514, 0x7000E444, 0x7000E4E4};
    EXPECT_EQ(expected, allowed);
    for (unsigned reg = T3R_REG_COUNT; reg < 64; ++reg) {
        EXPECT_EQ(0u, switch_t3r_address(reg));
        EXPECT_FALSE(switch_t3r_may_read(reg, 32));
    }
    for (unsigned reg = 0; reg < T3R_REG_COUNT; ++reg)
        for (unsigned width : {0u, 8u, 16u, 64u})
            EXPECT_FALSE(switch_t3r_may_read(reg, width));
}

TEST(T3R, TheSequenceHoldsEveryRegisterOnceAndTmrusTwiceAndNoIndexOutsideIt)
{
    std::vector<int> count(T3R_REG_COUNT, 0);
    for (unsigned i = 0; i < T3R_READ_COUNT; ++i)
        ++count[switch_t3r_sequence(i)];
    for (unsigned reg = 0; reg < T3R_REG_COUNT; ++reg)
        EXPECT_EQ(reg == T3R_TMRUS ? 2 : 1, count[reg]) << reg;
    EXPECT_EQ(static_cast<unsigned>(T3R_REG_COUNT), switch_t3r_sequence(T3R_READ_COUNT));
    EXPECT_EQ(static_cast<unsigned>(T3R_REG_COUNT), switch_t3r_sequence(1000));
}

TEST(T3R, AClosedPwmGateSkipsTheOneReadAndKeepsTheRest)
{
    struct Case {
        uint32_t rst, clk;
        unsigned gate;
    };
    const Case cases[] = {
        {0x049102CA, 0x9802D131, T3R_GATE_OPEN},
        {0x049102CA, 0x9800D131, T3R_GATE_CLOCK_OFF},
        {0x049302CA, 0x9802D131, T3R_GATE_IN_RESET},
        {0x049302CA, 0x9800D131, T3R_GATE_CLOCK_OFF},
    };
    for (const Case &c : cases) {
        Fake fake;
        fake.value[T3R_CAR_RST_DEVICES_L] = c.rst;
        fake.value[T3R_CAR_CLK_OUT_ENB_L] = c.clk;
        const switch_t3r_result result = fake.run();
        EXPECT_EQ(c.gate, result.gate);
        EXPECT_EQ(c.gate, switch_t3r_pwm_gate(c.rst, c.clk));
        const std::vector<unsigned> regs = fake.read_regs();
        const bool pwm_read = std::find(regs.begin(), regs.end(), static_cast<unsigned>(T3R_PWM_CSR_1)) != regs.end();
        EXPECT_EQ(c.gate == T3R_GATE_OPEN, pwm_read);
        EXPECT_EQ(c.gate == T3R_GATE_OPEN ? 22u : 21u, result.reads);
        EXPECT_EQ(0, result.stop);
        const bool skip_shown = std::find(fake.events.begin(), fake.events.end(),
                                          "show " + std::to_string(T3R_STEP_SKIP_PWM)) != fake.events.end();
        EXPECT_EQ(c.gate != T3R_GATE_OPEN, skip_shown);
        if (c.gate != T3R_GATE_OPEN)
            EXPECT_EQ(static_cast<unsigned>(T3R_PWM_NOT_CLASSIFIED), result.pwm);
    }
}

TEST(T3R, ThePwmWordIsClassifiedByWhatHekateWritesAndNothingElseIsKnown)
{
    auto classify = [](uint32_t word) {
        unsigned why = 99, duty = 99;
        const unsigned state = switch_t3r_pwm_classify(word, &why, &duty);
        return std::vector<unsigned>{state, why, duty};
    };
    const unsigned off = T3R_PWM_OFF_HEKATE, disabled = T3R_PWM_DISABLED, on = T3R_PWM_DUTY, unknown = T3R_PWM_UNKNOWN;
    EXPECT_EQ((std::vector<unsigned>{off, 0, 0}), classify(0x81000000));
    EXPECT_EQ((std::vector<unsigned>{disabled, 0, 0}), classify(0));
    // enable, field f, nothing else: duty 236 - f for f = 0 to 235
    EXPECT_EQ((std::vector<unsigned>{on, 0, 236}), classify(0x80000000));
    EXPECT_EQ((std::vector<unsigned>{on, 0, 1}), classify(0x80000000 | (235u << 16)));
    EXPECT_EQ((std::vector<unsigned>{on, 0, 185}), classify(0x80000000 | (51u << 16)));
    for (unsigned f = 0; f < 236; ++f)
        EXPECT_EQ((std::vector<unsigned>{on, 0, 236 - f}), classify(0x80000000 | (f << 16))) << f;
    // the special field 236 is duty 0 by the formula, which hekate writes as the off word
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_SPECIAL_ZERO_DUTY, 0}), classify(0x80000000 | (236u << 16)));
    // fields above 236
    for (unsigned f = 237; f < 256; ++f)
        EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_OUT_OF_RANGE, 0}), classify(0x80000000 | (f << 16))) << f;
    // the enable bit clear with anything else set
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_ENABLE_CLEAR_BITS, 0}), classify(0x01000000));
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_ENABLE_CLEAR_BITS, 0}), classify(0x00330000));
    // absolute 0 with another bit
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_ABSOLUTE_0_BITS, 0}), classify(0x81010000));
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_ABSOLUTE_0_BITS, 0}), classify(0x81000001));
    // bits outside the enable bit, bit 24 and the field
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_OTHER_BITS, 0}), classify(0x80000001));
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_OTHER_BITS, 0}), classify(0x8000FFFF));
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_OTHER_BITS, 0}), classify(0x82000000));
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_OTHER_BITS, 0}), classify(0xC0330000));
    EXPECT_EQ((std::vector<unsigned>{unknown, T3R_PWM_WHY_ALL_ONES, 0}), classify(0xFFFFFFFF));
}

TEST(T3R, ThePredictionsAreHekatesNyxExitStateAndOnlyThose)
{
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_MATCH), switch_t3r_expect(T3R_PINMUX_LCD_GPIO2, 0x74));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_DIFFERS), switch_t3r_expect(T3R_PINMUX_LCD_GPIO2, 0x01));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_MATCH), switch_t3r_expect(T3R_PINMUX_CAM1_PWDN, 0x59));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_DIFFERS), switch_t3r_expect(T3R_PINMUX_CAM1_PWDN, 0x51));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_MATCH), switch_t3r_expect(T3R_GPIO_A_OUT, 0x00));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_DIFFERS), switch_t3r_expect(T3R_GPIO_A_OUT, 0x20));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_MATCH), switch_t3r_expect(T3R_GPIO_CC_OUT, 0xEF));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_DIFFERS), switch_t3r_expect(T3R_GPIO_CC_OUT, 0x10));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_MATCH), switch_t3r_expect(T3R_GPIO_S_CNF, 0x80));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_MATCH), switch_t3r_expect(T3R_GPIO_S_OE, 0x00));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_DIFFERS), switch_t3r_expect(T3R_GPIO_S_OE, 0x80));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_MATCH), switch_t3r_expect(T3R_GPIO_V_CNF, 0x00));
    EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_DIFFERS), switch_t3r_expect(T3R_GPIO_V_CNF, 0x10));
    for (unsigned reg : {T3R_PINMUX_SATA_LED_ACTIVE, T3R_PINMUX_USB_VBUS_EN0, T3R_GPIO_A_CNF, T3R_GPIO_A_OE,
                         T3R_GPIO_CC_CNF, T3R_GPIO_CC_OE, T3R_GPIO_S_IN, T3R_GPIO_V_OE, T3R_PMC_NO_IOPOWER,
                         T3R_PMC_PWR_DET_VAL, T3R_TMRUS, T3R_CAR_RST_DEVICES_L, T3R_CAR_CLK_OUT_ENB_L})
        EXPECT_EQ(static_cast<unsigned>(T3R_EXPECT_NONE), switch_t3r_expect(reg, 0x12345678)) << reg;
}

TEST(T3R, AWordOfAllOnesEndsTheReadsAfterItsResultLineExceptTmrus)
{
    for (unsigned reg = 0; reg < T3R_REG_COUNT; ++reg) {
        if (reg == T3R_PWM_CSR_1)
            continue;
        Fake fake;
        fake.value[reg] = 0xFFFFFFFF;
        const switch_t3r_result result = fake.run();
        if (reg == T3R_TMRUS) {
            // a counter may read all ones; the run goes on (TMRUS did not advance only if the second read is equal)
            EXPECT_EQ(0, result.stop);
            EXPECT_EQ(22u, result.reads);
            continue;
        }
        EXPECT_EQ(static_cast<int>(SWITCH_T3R_STOP_ALL_ONES), result.stop) << reg;
        const std::vector<unsigned> regs = fake.read_regs();
        EXPECT_EQ(reg, regs.back()) << reg;
        EXPECT_NE(0u, fake.events[fake.events.size() - 2].rfind("read", 0)) << reg; // its RESULT line was shown
    }
    Fake fake;
    fake.value[T3R_PWM_CSR_1] = 0xFFFFFFFF;
    EXPECT_EQ(static_cast<int>(SWITCH_T3R_STOP_ALL_ONES), fake.run().stop);
}

TEST(T3R, TheReadBudgetEndsTheRunBeforeTheNextRead)
{
    for (int at = 0; at < 22; ++at) {
        Fake fake;
        fake.expire_at_check = at;
        const switch_t3r_result result = fake.run();
        EXPECT_EQ(static_cast<int>(SWITCH_T3R_TIME), result.stop) << at;
        EXPECT_EQ(static_cast<unsigned>(at), result.reads) << at;
        EXPECT_EQ("show " + std::to_string(T3R_STEP_COMPLETE), fake.events.back()) << at;
    }
    Fake never;
    EXPECT_EQ(0, never.run().stop);
    EXPECT_EQ(20u * 19200000u, switch_t3r_budget_ticks(19200000u));
    EXPECT_EQ(0u, switch_t3r_budget_ticks(19000000u));
    EXPECT_EQ(1920000u, switch_t3r_settle_ticks(19200000u));
    EXPECT_EQ(0u, switch_t3r_settle_ticks(1u));
}

TEST(T3R, TmrusAdvanceIsModuloThirtyTwoBitsAndZeroIsNotAdvance)
{
    EXPECT_TRUE(switch_t3r_advanced(10, 11));
    EXPECT_TRUE(switch_t3r_advanced(0xFFFFFFF0u, 0x10));
    EXPECT_FALSE(switch_t3r_advanced(5, 5));
    EXPECT_FALSE(switch_t3r_advanced(100, 99));
    Fake fake;
    fake.tmrus_second = fake.value[T3R_TMRUS];
    const switch_t3r_result result = fake.run();
    EXPECT_FALSE(result.tmrus_advanced);
    EXPECT_EQ(0, result.stop);
}

TEST(T3R, TheSummariesAreShownOnlyWhenAllTheReadsWereMade)
{
    Fake fake;
    fake.run();
    for (unsigned step : {T3R_STEP_CAR, T3R_STEP_FAN, T3R_STEP_ROUTE, T3R_STEP_RAIL, T3R_STEP_TACH, T3R_STEP_COMPLETE})
        EXPECT_NE(fake.events.end(), std::find(fake.events.begin(), fake.events.end(), "show " + std::to_string(step)));
    Fake stopped;
    stopped.value[T3R_GPIO_S_OE] = 0xFFFFFFFF;
    stopped.run();
    for (unsigned step : {T3R_STEP_CAR, T3R_STEP_FAN, T3R_STEP_ROUTE, T3R_STEP_RAIL, T3R_STEP_TACH})
        EXPECT_EQ(stopped.events.end(), std::find(stopped.events.begin(), stopped.events.end(), "show " + std::to_string(step)));
    EXPECT_EQ("show " + std::to_string(T3R_STEP_COMPLETE), stopped.events.back());
}

TEST(T3R, ScreenLinesNameTheAddressAndWidthBeforeAndTheValueAfterAndFitTheirCell)
{
    Fake fake;
    const switch_t3r_result result = fake.run();
    for (unsigned i = 0; i < 22; ++i) {
        switch_t3r_line begin, done;
        const std::string b = text_of(SWITCH_T3R_STEP_BEGIN(i), result, &begin);
        const std::string r = text_of(SWITCH_T3R_STEP_RESULT(i), result, &done);
        char address[9];
        snprintf(address, sizeof address, "%08X", switch_t3r_address(switch_t3r_sequence(i)));
        EXPECT_NE(std::string::npos, b.find(address)) << b;
        EXPECT_NE(std::string::npos, b.find("READ BEGIN 32 BIT")) << b;
        EXPECT_NE(std::string::npos, r.find(address)) << r;
        EXPECT_EQ(SWITCH_T3R_CELL, b.size()) << b;
        EXPECT_EQ(SWITCH_T3R_CELL, r.size()) << r;
        EXPECT_EQ(begin.row, done.row);
        EXPECT_EQ(begin.right, done.right);
        EXPECT_LT(begin.row, SWITCH_T3R_COLUMN_ROWS);
        EXPECT_EQ(i >= SWITCH_T3R_COLUMN_ROWS ? 1u : 0u, begin.right);
    }
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3R_STEP_RESULT(3), result).find("81000000 OFF AS HEKATE"));
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3R_STEP_RESULT(4), result).find("00000074 AS EXPECTED"));
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3R_STEP_RESULT(5), result).find("00000059 AS EXPECTED"));
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3R_STEP_RESULT(21), fake.drawn[SWITCH_T3R_STEP_RESULT(21)]).find("ADV 200000 US"));
    EXPECT_NE(std::string::npos, text_of(T3R_STEP_COMPLETE, result).find("READS 22. STOP 0000. NO WRITES"));
}

TEST(T3R, EveryScreenLineFitsAndNoLineSaysFanOnOrWrites)
{
    Fake fake;
    const switch_t3r_result result = fake.run();
    for (unsigned step = 0; step < T3R_STEP_COUNT; ++step) {
        const std::string text = text_of(step, result);
        EXPECT_LE(text.size(), static_cast<size_t>(SWITCH_DIAG_LINE_MAX)) << step;
        EXPECT_EQ(std::string::npos, text.find("FAN ON")) << text;
        EXPECT_EQ(std::string::npos, text.find("WRITE ")) << text;
    }
}

TEST(T3R, ASkippedPwmReadIsShownRedAndSaysWhy)
{
    Fake fake;
    fake.value[T3R_CAR_CLK_OUT_ENB_L] = 0x9800D131;
    const switch_t3r_result result = fake.run();
    switch_t3r_line line;
    const std::string text = text_of(T3R_STEP_SKIP_PWM, result, &line);
    EXPECT_NE(std::string::npos, text.find("NOT READ. CLOCK OFF"));
    EXPECT_TRUE(line.bad);
    EXPECT_NE(std::string::npos, text_of(T3R_STEP_FAN, result).find("NOT READ. PWM CLOCK OFF"));
}

TEST(T3R, AnUnknownPwmWordIsRedWithItsReason)
{
    Fake fake;
    fake.value[T3R_PWM_CSR_1] = 0x80000001;
    const switch_t3r_result result = fake.run();
    switch_t3r_line line;
    const std::string text = text_of(T3R_STEP_FAN, result, &line);
    EXPECT_NE(std::string::npos, text.find("UNKNOWN"));
    EXPECT_NE(std::string::npos, text.find("BITS OUTSIDE ENABLE"));
    EXPECT_TRUE(line.bad);
}

TEST(T3R, TheTimerLineIsDerivedBeforeItIsDrawnWithTheRawValuesOfTheFirstRun)
{
    Fake fake;
    fake.value[T3R_TMRUS] = 0x0680CFDD;
    fake.tmrus_second = 0x06C1B6A5;
    fake.run();
    const switch_t3r_result &shown = fake.drawn[SWITCH_T3R_STEP_RESULT(21)];
    EXPECT_EQ(0x06C1B6A5u, shown.tmrus_second);
    EXPECT_TRUE(shown.tmrus_advanced);
    const std::string text = text_of(SWITCH_T3R_STEP_RESULT(21), shown);
    EXPECT_NE(std::string::npos, text.find("06C1B6A5"));
    EXPECT_NE(std::string::npos, text.find("ADV 4253384 US"));
    EXPECT_EQ(std::string::npos, text.find("DID NOT ADVANCE"));
    EXPECT_FALSE(fake.drawn[SWITCH_T3R_STEP_BEGIN(21)].tmrus_advanced);
}

TEST(T3R, ATimerThatDidNotMoveIsStillShownAsSuchAtDrawTime)
{
    Fake fake;
    fake.tmrus_second = fake.value[T3R_TMRUS];
    fake.run();
    switch_t3r_line line;
    const std::string text = text_of(SWITCH_T3R_STEP_RESULT(21), fake.drawn[SWITCH_T3R_STEP_RESULT(21)], &line);
    EXPECT_NE(std::string::npos, text.find("DID NOT ADVANCE"));
    EXPECT_TRUE(line.bad);
}

TEST(T3R, TheCarAndPwmResultLinesHoldTheirTagsWhenTheyAreDrawn)
{
    Fake fake;
    fake.value[T3R_PWM_CSR_1] = 0;
    fake.run();
    switch_t3r_line clk;
    const std::string clk_text = text_of(SWITCH_T3R_STEP_RESULT(2), fake.drawn[SWITCH_T3R_STEP_RESULT(2)], &clk);
    EXPECT_NE(std::string::npos, clk_text.find("PWM CLOCK ON"));
    EXPECT_FALSE(clk.bad);
    const std::string pwm = text_of(SWITCH_T3R_STEP_RESULT(3), fake.drawn[SWITCH_T3R_STEP_RESULT(3)]);
    EXPECT_NE(std::string::npos, pwm.find("00000000 DISABLED"));
    EXPECT_EQ(static_cast<unsigned>(T3R_GATE_OPEN), fake.drawn[SWITCH_T3R_STEP_RESULT(2)].gate);
}

TEST(T3R, TheCarLineShowsBothRawWordsAndTheGate)
{
    Fake fake;
    fake.run();
    switch_t3r_line line;
    std::string text = text_of(T3R_STEP_CAR, fake.drawn[T3R_STEP_CAR], &line);
    EXPECT_NE(std::string::npos, text.find("RST_DEV_L 049102CA"));
    EXPECT_NE(std::string::npos, text.find("CLK_ENB_L 9802D131"));
    EXPECT_NE(std::string::npos, text.find("GATE OPEN"));
    EXPECT_FALSE(line.bad);
    Fake closed;
    closed.value[T3R_CAR_CLK_OUT_ENB_L] = 0x9800D131;
    closed.run();
    text = text_of(T3R_STEP_CAR, closed.drawn[T3R_STEP_CAR], &line);
    EXPECT_NE(std::string::npos, text.find("CLK_ENB_L 9800D131"));
    EXPECT_NE(std::string::npos, text.find("GATE CLOSED"));
    EXPECT_TRUE(line.bad);
}

namespace {
struct Rect {
    unsigned step, row, right, length;
    bool read;
};

std::vector<Rect> layout(Fake &fake)
{
    std::vector<Rect> rects;
    for (const auto &entry : fake.drawn) {
        switch_t3r_line line;
        switch_t3r_screen_line(entry.first, &entry.second, &line);
        const bool read = entry.first >= SWITCH_T3R_STEP_BEGIN(0) && entry.first < T3R_STEP_SKIP_PWM;
        rects.push_back({entry.first, line.row, line.right, static_cast<unsigned>(std::string(line.text.text).size()), read});
    }
    return rects;
}

std::vector<Fake> variants()
{
    std::vector<Fake> all(5);
    all[1].value[T3R_PWM_CSR_1] = 0;
    all[2].value[T3R_PWM_CSR_1] = 0x8000FFFF;
    all[3].value[T3R_CAR_CLK_OUT_ENB_L] = 0x9800D131;
    all[4].value[T3R_CAR_RST_DEVICES_L] = 0x049302CA;
    all[4].value[T3R_GPIO_A_OUT] = 0x20;
    return all;
}
} // namespace

TEST(T3R, ReadLinesStayInTheirTwoColumnsAndSummariesInTheirOwnRowsBelow)
{
    for (Fake &fake : variants()) {
        fake.run();
        std::set<std::pair<unsigned, unsigned>> read_cells;
        std::map<unsigned, unsigned> summary_rows;
        for (const Rect &r : layout(fake)) {
            if (r.read) {
                EXPECT_LT(r.row, SWITCH_T3R_COLUMN_ROWS) << r.step;
                EXPECT_LE(r.length, SWITCH_T3R_CELL) << r.step;
                if (r.step % 2 == 0)
                    EXPECT_TRUE(read_cells.insert({r.row, r.right}).second) << "two reads share a cell " << r.step;
            } else if (r.step >= T3R_STEP_CAR) {
                EXPECT_GE(r.row, static_cast<unsigned>(SWITCH_T3R_SUMMARY_FIRST_ROW)) << r.step;
                EXPECT_EQ(0u, r.right) << r.step;
                EXPECT_LE(r.length, static_cast<unsigned>(SWITCH_DIAG_LINE_MAX)) << r.step;
                EXPECT_TRUE(summary_rows.insert({r.row, r.step}).second) << "two summaries share row " << r.row;
            }
        }
        EXPECT_EQ(6u, summary_rows.size());
        EXPECT_EQ(static_cast<unsigned>(SWITCH_T3R_SUMMARY_FIRST_ROW) + 5u, summary_rows.rbegin()->first);
    }
}

TEST(T3R, ABeginLineAndItsResultLineShareACellAndNoLineUsesAGlyphTheFontLacks)
{
    const std::string font = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-.=/()_? ";
    for (Fake &fake : variants()) {
        fake.run();
        for (const auto &entry : fake.drawn) {
            switch_t3r_line line;
            switch_t3r_screen_line(entry.first, &entry.second, &line);
            for (char c : std::string(line.text.text))
                EXPECT_NE(std::string::npos, font.find(c)) << entry.first << " " << c;
        }
        for (unsigned i = 0; i < 22; ++i) {
            if (!fake.drawn.count(SWITCH_T3R_STEP_BEGIN(i)))
                continue;
            switch_t3r_line begin, done;
            switch_t3r_screen_line(SWITCH_T3R_STEP_BEGIN(i), &fake.drawn[SWITCH_T3R_STEP_BEGIN(i)], &begin);
            switch_t3r_screen_line(SWITCH_T3R_STEP_RESULT(i), &fake.drawn[SWITCH_T3R_STEP_RESULT(i)], &done);
            EXPECT_EQ(begin.row, done.row);
            EXPECT_EQ(begin.right, done.right);
        }
    }
}
