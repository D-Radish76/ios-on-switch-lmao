// T3's logic on the host: the allowlists, the state check, the order of reads and writes, the readback of every write, the
// off sequence after each kind of stop, the rail time limit inside the tach loop, the tach verdict and the screen lines.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../t3.h"
#include "../t3_report.h"
}

namespace {
constexpr uint32_t kTachBit = 1u << SWITCH_T3_TACH_BIT;

// One tick is one read of the tach pad, so a 2000 ms window holds 2000 samples and the rail limit is a count of samples.
struct Fake {
    uint32_t reg[T3_REG_COUNT] = {};
    std::vector<std::string> events;
    std::map<unsigned, switch_t3_result> drawn;
    std::set<uint32_t> ignored_values; // a write of such a value does not change the register
    int refuse_step = -1;
    int expire_reads_after = -1; // the nth call of read_expired and every later one answer yes
    int expire_checks = 0;
    uint64_t now = 0;
    uint64_t rail_start = 0;
    bool rail_marked = false;
    unsigned unmarked_rail_checks = 0;
    uint64_t rail_limit = 15000;
    uint64_t window_start = 0;
    uint64_t window_ticks = 0;
    bool no_clock = false;
    unsigned spin_period = 10; // samples per level change while the fan is driven
    unsigned idle_period = 0;  // samples per level change with no fan, 0 for none
    bool start_high = false;
    unsigned rail_off_ignored = 0;     // the first n sends of the rail off word do not change the register
    uint32_t rail_off_stores = ~0u;    // if set, a rail off send leaves this word in GPIO_A_OUT
    int jump_after_show = -1;          // showing this step moves the clock far past every limit
    int jump_after_write = -1;         // sending this write moves the clock far past every limit
    unsigned long long jump = 100000;

    Fake()
    {
        reg[T3_CAR_RST_DEVICES_L] = 0x049102CA;
        reg[T3_CAR_CLK_OUT_ENB_L] = 0x9802D131;
        reg[T3_PWM_CSR_1] = 0;
        reg[T3_PINMUX_LCD_GPIO2] = 0x74;
        reg[T3_PINMUX_CAM1_PWDN] = 0x59;
        reg[T3_PINMUX_SATA_LED_ACTIVE] = 0x01;
        reg[T3_PINMUX_USB_VBUS_EN0] = 0x101;
        reg[T3_GPIO_A_CNF] = 0x20;
        reg[T3_GPIO_A_OE] = 0x20;
        reg[T3_GPIO_CC_CNF] = 0x18;
        reg[T3_GPIO_CC_OE] = 0x18;
        reg[T3_GPIO_S_CNF] = 0x80;
        reg[T3_GPIO_V_CNF] = 0x06;
        reg[T3_GPIO_V_OE] = 0x07;
        reg[T3_PMC_PWR_DET_VAL] = 0x005C3C2C;
    }

    bool driven() const
    {
        return reg[T3_PWM_CSR_1] == SWITCH_T3_PWM_DUTY51 && reg[T3_PINMUX_LCD_GPIO2] == SWITCH_T3_ROUTE_PWM_WORD &&
               (reg[T3_GPIO_A_OUT] & 0x20);
    }

    uint32_t tach() const
    {
        const unsigned period = driven() ? spin_period : idle_period;
        const bool level = period ? (now / period) & 1u : start_high;
        return level ? kTachBit : 0;
    }

    static std::string name(const char *kind, unsigned a, long b = -1)
    {
        return std::string(kind) + " " + std::to_string(a) + (b >= 0 ? " " + std::to_string(b) : "");
    }

    static uint32_t read(void *c, unsigned r)
    {
        Fake *f = static_cast<Fake *>(c);
        if (r == T3_GPIO_S_IN) {
            ++f->now;
            return f->tach();
        }
        f->events.push_back(name("read", r));
        return f->reg[r];
    }
    static void write(void *c, unsigned r, uint32_t v)
    {
        Fake *f = static_cast<Fake *>(c);
        f->events.push_back(name("write", r, v));
        if (r == T3_GPIO_A_OUT && v == SWITCH_T3_RAIL_OFF_WORD && f->rail_off_ignored) {
            --f->rail_off_ignored;
            return;
        }
        if (r == T3_GPIO_A_OUT && v == SWITCH_T3_RAIL_OFF_WORD && f->rail_off_stores != ~0u)
            f->reg[r] = f->rail_off_stores;
        else if (!f->ignored_values.count(v))
            f->reg[r] = v;
        if (f->jump_after_write >= 0 && r == switch_t3_write_reg(f->jump_after_write) &&
            v == switch_t3_write_value(f->jump_after_write))
            f->now += f->jump;
    }
    static int show(void *c, unsigned step, const switch_t3_result *res)
    {
        Fake *f = static_cast<Fake *>(c);
        f->events.push_back(name("show", step));
        f->drawn[step] = *res;
        if (static_cast<int>(step) == f->jump_after_show)
            f->now += f->jump;
        return static_cast<int>(step) != f->refuse_step;
    }
    static int read_expired(void *c)
    {
        Fake *f = static_cast<Fake *>(c);
        return f->expire_reads_after >= 0 && f->expire_checks++ >= f->expire_reads_after;
    }
    static void rail_mark(void *c)
    {
        Fake *f = static_cast<Fake *>(c);
        f->events.push_back("rail_mark");
        f->rail_marked = true;
        f->rail_start = f->now;
    }
    static int rail_expired(void *c)
    {
        Fake *f = static_cast<Fake *>(c);
        f->unmarked_rail_checks += !f->rail_marked;
        return f->no_clock || f->now - f->rail_start >= f->rail_limit;
    }
    static void window_begin(void *c, unsigned ms)
    {
        Fake *f = static_cast<Fake *>(c);
        f->window_start = f->now;
        f->window_ticks = ms;
    }
    static int window_over(void *c)
    {
        Fake *f = static_cast<Fake *>(c);
        return f->no_clock || f->now - f->window_start >= f->window_ticks;
    }

    switch_t3_result run()
    {
        switch_t3_io io{this, read, write, show, read_expired, rail_mark, rail_expired, window_begin, window_over};
        switch_t3_result result;
        switch_t3_run(&io, &result);
        return result;
    }

    std::vector<std::string> writes() const
    {
        std::vector<std::string> out;
        for (const auto &e : events)
            if (e.rfind("write", 0) == 0)
                out.push_back(e);
        return out;
    }
    size_t first(const std::string &prefix) const
    {
        for (size_t i = 0; i < events.size(); ++i)
            if (events[i].rfind(prefix, 0) == 0)
                return i;
        return events.size();
    }
};

std::string write_event(unsigned w)
{
    return Fake::name("write", switch_t3_write_reg(w), switch_t3_write_value(w));
}

std::string text_of(unsigned step, const switch_t3_result &result)
{
    switch_t3_line line;
    switch_t3_screen_line(step, &result, &line);
    return line.text.text;
}
} // namespace

TEST(T3, OnlyTheSixReviewedWordsPassTheWriteAllowlist)
{
    const uint32_t probes[] = {0, 1, 0x20, 0x74, 0x80000000u, 0x80a00000u, 0x80b90000u, 0x81000000u, 0xffffffffu, 0x00000101};
    unsigned allowed = 0;
    for (unsigned reg = 0; reg < T3_REG_COUNT + 3; ++reg)
        for (uint32_t value : probes)
            allowed += switch_t3_may_write(reg, value);
    EXPECT_EQ(6u, allowed);
    for (unsigned w = 0; w < T3_WRITE_COUNT; ++w)
        EXPECT_TRUE(switch_t3_may_write(switch_t3_write_reg(w), switch_t3_write_value(w))) << w;
    EXPECT_EQ(switch_t3_write_value(T3_W_PWM_DUTY), 0x80B90000u);
    EXPECT_EQ(switch_t3_write_value(T3_O_PWM_OFF), 0x81000000u);
    EXPECT_FALSE(switch_t3_may_write(T3_PWM_CSR_1, 0x80a00000u)); // duty 76 is not in this stage
    EXPECT_FALSE(switch_t3_may_write(T3_PINMUX_LCD_GPIO2, 0x20u));
    EXPECT_FALSE(switch_t3_may_write(T3_GPIO_CC_OUT, 0));
    EXPECT_FALSE(switch_t3_may_write(T3_PMC_NO_IOPOWER, 0));
    EXPECT_FALSE(switch_t3_may_write(T3_CAR_RST_DEVICES_L, 0));
}

TEST(T3, WritesTouchOnlyThreeRegisters)
{
    std::set<unsigned> regs;
    for (unsigned w = 0; w < T3_WRITE_COUNT; ++w)
        regs.insert(switch_t3_write_reg(w));
    EXPECT_EQ(3u, regs.size());
    EXPECT_TRUE(regs.count(T3_GPIO_A_OUT));
    EXPECT_TRUE(regs.count(T3_PWM_CSR_1));
    EXPECT_TRUE(regs.count(T3_PINMUX_LCD_GPIO2));
    EXPECT_EQ(0x6000D020u, switch_t3_address(T3_GPIO_A_OUT));
    EXPECT_EQ(0x7000A010u, switch_t3_address(T3_PWM_CSR_1));
    EXPECT_EQ(0x7000320Cu, switch_t3_address(T3_PINMUX_LCD_GPIO2));
}

TEST(T3, ReadAllowlistHoldsTwentyRegistersAtThirtyTwoBitsOnly)
{
    std::set<uint32_t> addresses;
    for (unsigned reg = 0; reg < T3_REG_COUNT; ++reg) {
        EXPECT_TRUE(switch_t3_may_read(reg, 32));
        EXPECT_FALSE(switch_t3_may_read(reg, 16));
        EXPECT_FALSE(switch_t3_may_read(reg, 8));
        EXPECT_NE(0u, switch_t3_address(reg));
        addresses.insert(switch_t3_address(reg));
    }
    EXPECT_EQ(20u, addresses.size());
    EXPECT_FALSE(switch_t3_may_read(T3_REG_COUNT, 32));
    EXPECT_EQ(0u, switch_t3_address(T3_REG_COUNT));
    EXPECT_FALSE(addresses.count(0x60005010u)); // no timer read
    EXPECT_FALSE(addresses.count(0x60006000u));
}

TEST(T3, StateCheckAcceptsTheMeasuredEntryStateWhateverTheTachLevelIs)
{
    Fake fake;
    EXPECT_EQ(T3_STATE_OK, switch_t3_state_check(fake.reg));
    fake.reg[T3_GPIO_S_IN] = kTachBit;
    EXPECT_EQ(T3_STATE_OK, switch_t3_state_check(fake.reg));
}

TEST(T3, StateCheckNamesEachDifference)
{
    struct Case {
        unsigned reg;
        uint32_t value;
        unsigned state;
    };
    const Case cases[] = {
        {T3_CAR_CLK_OUT_ENB_L, 0, T3_STATE_GATE},
        {T3_CAR_RST_DEVICES_L, 0x00020000, T3_STATE_GATE},
        {T3_PWM_CSR_1, 0x81000000, T3_STATE_PWM},
        {T3_PINMUX_LCD_GPIO2, 0x01, T3_STATE_LCD_PAD},
        {T3_PINMUX_CAM1_PWDN, 0x58, T3_STATE_TACH_PAD},
        {T3_PINMUX_SATA_LED_ACTIVE, 0x00, T3_STATE_RAIL_PIN},
        {T3_PINMUX_USB_VBUS_EN0, 0x100, T3_STATE_USB_PIN},
        {T3_GPIO_A_CNF, 0, T3_STATE_A5},
        {T3_GPIO_A_OE, 0, T3_STATE_A5},
        {T3_GPIO_A_OUT, 0x20, T3_STATE_A5},
        {T3_GPIO_A_OUT, 0x01, T3_STATE_A5},
        {T3_GPIO_CC_CNF, 0, T3_STATE_CC4},
        {T3_GPIO_CC_OE, 0, T3_STATE_CC4},
        {T3_GPIO_CC_OUT, 0x10, T3_STATE_CC4},
        {T3_GPIO_S_CNF, 0, T3_STATE_S7},
        {T3_GPIO_S_OE, 0x80, T3_STATE_S7},
        {T3_GPIO_V_CNF, 0x16, T3_STATE_V4},
        {T3_PMC_NO_IOPOWER, 1, T3_STATE_PMC},
        {T3_PMC_PWR_DET_VAL, 0x005C3C2C | (1u << 21), T3_STATE_PMC},
    };
    for (const Case &c : cases) {
        Fake fake;
        fake.reg[c.reg] = c.value;
        EXPECT_EQ(c.state, switch_t3_state_check(fake.reg)) << c.reg << " " << c.value;
    }
}

TEST(T3, ADifferentStateStopsBeforeAnyWrite)
{
    Fake fake;
    fake.reg[T3_PWM_CSR_1] = 0x81000000;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_STATE, result.stop);
    EXPECT_EQ(T3_STATE_PWM, result.state);
    EXPECT_EQ(0u, result.writes);
    EXPECT_EQ(0u, result.issued);
    EXPECT_TRUE(fake.writes().empty());
    EXPECT_EQ(0x81000000u, fake.reg[T3_PWM_CSR_1]);
}

TEST(T3, EveryStateWordIsReadBeforeTheFirstWriteAndTheFirstWriteIsTheRail)
{
    Fake fake;
    const switch_t3_result result = fake.run();
    std::set<unsigned> before;
    for (size_t i = 0; i < fake.first("write"); ++i)
        if (fake.events[i].rfind("read", 0) == 0)
            before.insert(static_cast<unsigned>(std::stoi(fake.events[i].substr(5))));
    for (unsigned reg = 0; reg < T3_REG_COUNT; ++reg)
        if (reg != T3_GPIO_S_IN) {
            EXPECT_TRUE(before.count(reg)) << reg;
        }
    EXPECT_EQ(write_event(T3_W_RAIL_ON), fake.writes().front());
    EXPECT_EQ(result.value[T3_PWM_CSR_1], 0u);
}

TEST(T3, ThePassingRunWritesInOrderReadsEachBackAndEndsWithEverythingOff)
{
    Fake fake;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(0, result.stop);
    EXPECT_EQ(T3_VERDICT_PASS, result.verdict);
    EXPECT_EQ(6u, result.writes);
    EXPECT_EQ(0u, result.mismatched);
    const std::vector<std::string> order = {write_event(T3_W_RAIL_ON), write_event(T3_W_PWM_DUTY), write_event(T3_W_ROUTE_PWM),
                                            write_event(T3_O_PWM_OFF), write_event(T3_O_RAIL_OFF), write_event(T3_O_PARK_PAD)};
    EXPECT_EQ(order, fake.writes());
    for (size_t i = 0; i < fake.events.size(); ++i) {
        if (fake.events[i].rfind("write", 0) != 0)
            continue;
        const std::string reg = fake.events[i].substr(6, fake.events[i].find(' ', 6) - 6);
        EXPECT_EQ("read " + reg, fake.events[i + 1]) << fake.events[i];
    }
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
    EXPECT_EQ(0x81000000u, fake.reg[T3_PWM_CSR_1]);
    EXPECT_EQ(0x74u, fake.reg[T3_PINMUX_LCD_GPIO2]);
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), result.rail);
    for (unsigned w = 0; w < T3_WRITE_COUNT; ++w)
        EXPECT_EQ(switch_t3_write_value(w), result.readback[w]) << w;
}

TEST(T3, EachWriteIsAnnouncedOnTheScreenBeforeItIsSent)
{
    Fake fake;
    fake.run();
    EXPECT_LT(fake.first("show " + std::to_string(SWITCH_T3_STEP_BEGIN(T3_PHASE_RAIL))), fake.first("write"));
    const std::string pwm = write_event(T3_W_PWM_DUTY);
    EXPECT_LT(fake.first("show " + std::to_string(SWITCH_T3_STEP_BEGIN(T3_PHASE_SPIN))), fake.first(pwm));
    EXPECT_LT(fake.first(pwm), fake.first("show " + std::to_string(SWITCH_T3_STEP_DONE(T3_PHASE_SPIN))));
}

TEST(T3, ARailReadbackMismatchSendsNoPwmAndUndoesOnlyTheRail)
{
    Fake fake;
    fake.ignored_values = {SWITCH_T3_RAIL_ON_WORD};
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_READBACK, result.stop);
    EXPECT_TRUE(result.mismatched & 1u << T3_W_RAIL_ON);
    const std::vector<std::string> expected = {write_event(T3_W_RAIL_ON), write_event(T3_O_RAIL_OFF)};
    EXPECT_EQ(expected, fake.writes());
    EXPECT_FALSE(result.window[T3_WINDOW_RAIL].ran);
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), result.rail);
}

TEST(T3, APwmReadbackMismatchSkipsTheRouteAndUndoesPwmAndRail)
{
    Fake fake;
    fake.ignored_values = {SWITCH_T3_PWM_DUTY51};
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_READBACK, result.stop);
    const std::vector<std::string> expected = {write_event(T3_W_RAIL_ON), write_event(T3_W_PWM_DUTY), write_event(T3_O_PWM_OFF),
                                               write_event(T3_O_RAIL_OFF)};
    EXPECT_EQ(expected, fake.writes());
    EXPECT_EQ(0x81000000u, fake.reg[T3_PWM_CSR_1]);
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
}

TEST(T3, AMismatchDuringTheOffSequenceDoesNotStopTheRemainingOffWrites)
{
    Fake fake;
    fake.ignored_values = {SWITCH_T3_PWM_OFF_WORD};
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_READBACK, result.stop);
    EXPECT_TRUE(result.mismatched & 1u << T3_O_PWM_OFF);
    const std::vector<std::string> tail = {write_event(T3_O_PWM_OFF), write_event(T3_O_RAIL_OFF), write_event(T3_O_PARK_PAD)};
    const std::vector<std::string> all = fake.writes();
    ASSERT_EQ(6u, all.size());
    EXPECT_EQ(tail, std::vector<std::string>(all.begin() + 3, all.end()));
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
    EXPECT_EQ(0x74u, fake.reg[T3_PINMUX_LCD_GPIO2]);
}

TEST(T3, EveryOffWriteStillRunsWhenAllOfThemReadBackWrong)
{
    Fake fake;
    fake.ignored_values = {SWITCH_T3_PWM_OFF_WORD, SWITCH_T3_RAIL_OFF_WORD, SWITCH_T3_PARK_PAD_WORD};
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_RAIL_NOT_OFF, result.stop);
    EXPECT_EQ(6u + SWITCH_T3_OFF_RETRIES, result.writes);
    EXPECT_EQ(7u << T3_O_PWM_OFF, result.mismatched & (7u << T3_O_PWM_OFF));
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_MAYBE_ON), result.rail);
}

TEST(T3, TheRailLimitCutsTheTachWindowItselfAndTheOffSequenceFollows)
{
    Fake fake;
    fake.rail_limit = 500;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_RAIL_TIME, result.stop);
    EXPECT_EQ(static_cast<unsigned>(T3_CUT_RAIL), result.window[T3_WINDOW_RAIL].cut);
    EXPECT_LE(result.window[T3_WINDOW_RAIL].samples, 502u);
    EXPECT_FALSE(result.window[T3_WINDOW_SPIN].ran);
    EXPECT_EQ(std::vector<std::string>({write_event(T3_W_RAIL_ON), write_event(T3_O_RAIL_OFF)}), fake.writes());
}

TEST(T3, TheRailLimitCutsTheSpinWindowToo)
{
    Fake fake;
    fake.rail_limit = 3500; // the rail window and the spin-up take 3000 samples
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_RAIL_TIME, result.stop);
    EXPECT_EQ(static_cast<unsigned>(T3_CUT_RAIL), result.window[T3_WINDOW_SPIN].cut);
    EXPECT_LE(result.window[T3_WINDOW_SPIN].samples, 502u);
    EXPECT_EQ(6u, result.writes);
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
    EXPECT_EQ(0x74u, fake.reg[T3_PINMUX_LCD_GPIO2]);
}

TEST(T3, AnExpiredRailClockAfterTheRailWriteStopsBeforeAnyPwmWrite)
{
    Fake fake;
    fake.no_clock = true;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_RAIL_TIME, result.stop);
    EXPECT_EQ(std::vector<std::string>({write_event(T3_W_RAIL_ON), write_event(T3_O_RAIL_OFF)}), fake.writes());
}

TEST(T3, TheOffSequenceRunsWhenTheScreenStopsDrawingWithTheRailOn)
{
    Fake fake;
    fake.refuse_step = SWITCH_T3_STEP_DONE(T3_PHASE_RAIL);
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_NOT_SHOWN, result.stop);
    EXPECT_EQ(std::vector<std::string>({write_event(T3_W_RAIL_ON), write_event(T3_O_RAIL_OFF)}), fake.writes());
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
}

TEST(T3, ARefusedAnnouncementBeforeTheRailSendsNothing)
{
    Fake fake;
    fake.refuse_step = SWITCH_T3_STEP_BEGIN(T3_PHASE_RAIL);
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_NOT_SHOWN, result.stop);
    EXPECT_TRUE(fake.writes().empty());
}

TEST(T3, TheStateReadsHaveATimeBudgetAndAnAllOnesWordIsNotState)
{
    Fake budget;
    budget.expire_reads_after = 5;
    EXPECT_EQ(SWITCH_T3_TIME, budget.run().stop);
    EXPECT_TRUE(budget.writes().empty());
    Fake ones;
    ones.reg[T3_PMC_NO_IOPOWER] = 0xffffffffu;
    EXPECT_EQ(SWITCH_T3_STOP_ALL_ONES, ones.run().stop);
    EXPECT_TRUE(ones.writes().empty());
}

TEST(T3, TheTachCountsChangesWhateverLevelItStartsAt)
{
    for (bool high : {false, true}) {
        Fake fake;
        fake.start_high = high;
        const switch_t3_result result = fake.run();
        EXPECT_EQ(high ? 1u : 0u, result.baseline_level) << high;
        EXPECT_EQ(0u, result.window[T3_WINDOW_IDLE].transitions);
        EXPECT_EQ(0u, result.window[T3_WINDOW_RAIL].transitions);
        EXPECT_GT(result.window[T3_WINDOW_SPIN].transitions, 100u);
        EXPECT_EQ(T3_VERDICT_PASS, result.verdict);
    }
}

TEST(T3, NoChangesAtDutyFiftyOneIsNoPassButTheOffSequenceStillRuns)
{
    Fake fake;
    fake.spin_period = 0;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(T3_VERDICT_NO_EDGES, result.verdict);
    EXPECT_EQ(SWITCH_T3_NO_EDGES, result.stop);
    EXPECT_EQ(6u, result.writes);
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
}

TEST(T3, ATachThatChangesAsMuchBeforeTheFanIsNoPass)
{
    Fake fake;
    fake.idle_period = 10;
    fake.spin_period = 10;
    const switch_t3_result result = fake.run();
    EXPECT_GT(result.window[T3_WINDOW_IDLE].transitions, 100u);
    EXPECT_EQ(T3_VERDICT_NO_EDGES, result.verdict);
}

TEST(T3, WindowsWithoutASampleCountAreNoVerdict)
{
    Fake fake;
    fake.no_clock = true;
    fake.rail_limit = ~0ull;
    switch_t3_io io{&fake, Fake::read, Fake::write, Fake::show, Fake::read_expired, Fake::rail_mark,
                    [](void *) { return 0; }, Fake::window_begin, [](void *) { return 1; }};
    switch_t3_result result;
    switch_t3_run(&io, &result);
    EXPECT_EQ(T3_VERDICT_FEW_SAMPLES, result.verdict);
    EXPECT_EQ(SWITCH_T3_SAMPLES, result.stop);
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
}

TEST(T3, VerdictNeedsFourTransitionsAndMoreThanBothEarlierWindows)
{
    switch_t3_window w[T3_WINDOW_COUNT] = {};
    for (auto &x : w) {
        x.ran = 1;
        x.samples = 5000;
    }
    w[T3_WINDOW_SPIN].transitions = 3;
    EXPECT_EQ(T3_VERDICT_NO_EDGES, switch_t3_verdict(w));
    w[T3_WINDOW_SPIN].transitions = 4;
    EXPECT_EQ(T3_VERDICT_PASS, switch_t3_verdict(w));
    w[T3_WINDOW_RAIL].transitions = 4;
    EXPECT_EQ(T3_VERDICT_NO_EDGES, switch_t3_verdict(w));
    w[T3_WINDOW_SPIN].samples = 10;
    EXPECT_EQ(T3_VERDICT_FEW_SAMPLES, switch_t3_verdict(w));
}

TEST(T3, MsTicksAreZeroUnlessTheCounterIsThe19200kHzOne)
{
    EXPECT_EQ(19200000u * 2, switch_t3_ms_ticks(19200000, 2000));
    EXPECT_EQ(0u, switch_t3_ms_ticks(0, 2000));
    EXPECT_EQ(0u, switch_t3_ms_ticks(24000000, 2000));
}

TEST(T3, NoLineUsesAGlyphTheFontLacksAndEveryLineFitsItsRow)
{
    const std::string font = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-.=/()_? ";
    std::vector<Fake> variants(5);
    variants[1].reg[T3_PWM_CSR_1] = 1;
    variants[2].ignored_values = {SWITCH_T3_PWM_OFF_WORD};
    variants[3].rail_limit = 500;
    variants[4].spin_period = 0;
    for (Fake &fake : variants) {
        fake.run();
        for (const auto &entry : fake.drawn) {
            switch_t3_line line;
            switch_t3_screen_line(entry.first, &entry.second, &line);
            EXPECT_LT(line.row, SWITCH_T3_LINE_ROWS);
            EXPECT_EQ(static_cast<unsigned>(SWITCH_DIAG_LINE_MAX), line.text.length) << entry.first;
            for (char c : std::string(line.text.text))
                EXPECT_NE(std::string::npos, font.find(c)) << entry.first << " " << c;
        }
    }
}

TEST(T3, ABeginLineAndItsDoneLineShareARow)
{
    Fake fake;
    fake.run();
    for (unsigned p = 0; p < T3_PHASE_COUNT; ++p) {
        if (!fake.drawn.count(SWITCH_T3_STEP_BEGIN(p)) || !fake.drawn.count(SWITCH_T3_STEP_DONE(p)))
            continue;
        switch_t3_line a, b;
        switch_t3_screen_line(SWITCH_T3_STEP_BEGIN(p), &fake.drawn[SWITCH_T3_STEP_BEGIN(p)], &a);
        switch_t3_screen_line(SWITCH_T3_STEP_DONE(p), &fake.drawn[SWITCH_T3_STEP_DONE(p)], &b);
        EXPECT_EQ(a.row, b.row) << p;
    }
}

TEST(T3, TheLinesSayWhatHappened)
{
    Fake mismatch;
    mismatch.reg[T3_PWM_CSR_1] = 0x81000000;
    const switch_t3_result stopped = mismatch.run();
    const std::string state = text_of(SWITCH_T3_STEP_DONE(T3_PHASE_STATE), mismatch.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_STATE)]);
    EXPECT_NE(std::string::npos, state.find("DIFFERS"));
    EXPECT_NE(std::string::npos, state.find("NO WRITE WAS MADE"));
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3_STEP_COMPLETE, stopped).find("NO PASS"));

    Fake passing;
    const switch_t3_result passed = passing.run();
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3_STEP_COMPLETE, passed).find("PASS: MORE TRANSITIONS"));
    const std::string off = text_of(SWITCH_T3_STEP_DONE(T3_PHASE_OFF), passing.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_OFF)]);
    EXPECT_NE(std::string::npos, off.find("81000000 OK"));

    Fake wrong;
    wrong.ignored_values = {SWITCH_T3_RAIL_OFF_WORD};
    wrong.run();
    EXPECT_NE(std::string::npos,
              text_of(SWITCH_T3_STEP_DONE(T3_PHASE_OFF), wrong.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_OFF)]).find("READBACK DIFFERS"));
}

TEST(T3, ARailThatReadsBackOnAfterTheOffWriteIsRetriedAndNeverCalledOff)
{
    Fake fake;
    fake.rail_off_ignored = 100;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_MAYBE_ON), result.rail);
    EXPECT_EQ(SWITCH_T3_RAIL_NOT_OFF, result.stop);
    unsigned off_writes = 0;
    for (const auto &w : fake.writes())
        off_writes += w == write_event(T3_O_RAIL_OFF);
    EXPECT_EQ(1u + SWITCH_T3_OFF_RETRIES, off_writes);
    EXPECT_EQ(write_event(T3_O_PARK_PAD), fake.writes()[fake.writes().size() - 1 - SWITCH_T3_OFF_RETRIES]);
    EXPECT_EQ(0x81000000u, fake.reg[T3_PWM_CSR_1]);
    EXPECT_EQ(0x74u, fake.reg[T3_PINMUX_LCD_GPIO2]);
    EXPECT_FALSE(result.window[T3_WINDOW_STOPPED].ran);
}

TEST(T3, ARailOffReadbackOfSomethingElseIsUnknownNotOn)
{
    Fake fake;
    fake.rail_off_stores = 0x01;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_UNKNOWN), result.rail);
    EXPECT_EQ(SWITCH_T3_RAIL_NOT_OFF, result.stop);
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3_STEP_DONE(T3_PHASE_OFF), fake.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_OFF)]).find("RAIL STATE UNKNOWN"));
}

TEST(T3, AnOffWordThatSticksOnTheRetryEndsAsConfirmedOffWithTheMismatchKept)
{
    Fake fake;
    fake.rail_off_ignored = 1;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), result.rail);
    EXPECT_EQ(SWITCH_T3_READBACK, result.stop);
    EXPECT_TRUE(result.mismatched & 1u << T3_O_RAIL_OFF);
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
}

TEST(T3, TheRailIsNotOffUntilItsOffReadbackSaysSo)
{
    Fake fake;
    fake.run();
    for (const auto &entry : fake.drawn) {
        if (entry.first >= SWITCH_T3_STEP_DONE(T3_PHASE_RAIL) && entry.first < SWITCH_T3_STEP_DONE(T3_PHASE_OFF)) {
            EXPECT_EQ(static_cast<unsigned>(T3_RAIL_MAYBE_ON), entry.second.rail) << entry.first;
        }
    }
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), fake.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_OFF)].rail);
}

TEST(T3, TheRailLimitIsLookedAtAfterADrawAndNoOnWriteFollowsIt)
{
    Fake fake;
    fake.jump_after_show = SWITCH_T3_STEP_DONE(T3_PHASE_RAIL);
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_RAIL_TIME, result.stop);
    EXPECT_EQ(std::vector<std::string>({write_event(T3_W_RAIL_ON), write_event(T3_O_RAIL_OFF)}), fake.writes());
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), result.rail);
}

TEST(T3, TheRailLimitIsLookedAtBetweenTwoBackToBackOnWrites)
{
    Fake fake;
    fake.jump_after_write = T3_W_PWM_DUTY;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_RAIL_TIME, result.stop);
    EXPECT_EQ(0u, result.issued & 1u << T3_W_ROUTE_PWM);
    const std::vector<std::string> expected = {write_event(T3_W_RAIL_ON), write_event(T3_W_PWM_DUTY), write_event(T3_O_PWM_OFF),
                                               write_event(T3_O_RAIL_OFF)};
    EXPECT_EQ(expected, fake.writes());
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), result.rail);
    EXPECT_EQ(0x81000000u, fake.reg[T3_PWM_CSR_1]);
}

TEST(T3, ASpentLimitDoesNotSkipAnyOffWrite)
{
    Fake fake;
    fake.jump_after_write = T3_W_ROUTE_PWM;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(SWITCH_T3_RAIL_TIME, result.stop);
    EXPECT_EQ(6u, result.writes);
    EXPECT_EQ(0u, fake.reg[T3_GPIO_A_OUT]);
    EXPECT_EQ(0x74u, fake.reg[T3_PINMUX_LCD_GPIO2]);
    EXPECT_EQ(0x81000000u, fake.reg[T3_PWM_CSR_1]);
}

TEST(T3, TheRailLineSaysWhatIsKnown)
{
    Fake fake;
    fake.run();
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3_STEP_DONE(T3_PHASE_OFF), fake.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_OFF)]).find("RAIL OFF CONFIRMED"));
    Fake stuck;
    stuck.rail_off_ignored = 100;
    stuck.run();
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3_STEP_DONE(T3_PHASE_OFF), stuck.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_OFF)]).find("RAIL MAY BE ON"));
}

TEST(T3, TheRailIsUncheckedUntilTheStateReadsSucceed)
{
    Fake fake;
    const switch_t3_result result = fake.run();
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_UNCHECKED), fake.drawn[SWITCH_T3_STEP_START].rail);
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_UNCHECKED), fake.drawn[SWITCH_T3_STEP_BEGIN(T3_PHASE_STATE)].rail);
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), fake.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_STATE)].rail);
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), result.rail);
}

TEST(T3, AFailedStateReadLeavesTheRailUncheckedAndWritesNothing)
{
    Fake mismatch;
    mismatch.reg[T3_GPIO_A_OUT] = 0x20; // the rail already reads on
    const switch_t3_result a = mismatch.run();
    Fake budget;
    budget.expire_reads_after = 5;
    const switch_t3_result b = budget.run();
    Fake ones;
    ones.reg[T3_PMC_NO_IOPOWER] = 0xffffffffu;
    const switch_t3_result c = ones.run();
    for (const switch_t3_result *r : {&a, &b, &c}) {
        EXPECT_EQ(static_cast<unsigned>(T3_RAIL_UNCHECKED), r->rail);
        EXPECT_EQ(0u, r->issued);
        EXPECT_EQ(0u, r->writes);
        EXPECT_NE(SWITCH_T3_RAIL_NOT_OFF, r->stop);
    }
    EXPECT_EQ(SWITCH_T3_STATE, a.stop);
    EXPECT_EQ(SWITCH_T3_TIME, b.stop);
    EXPECT_EQ(SWITCH_T3_STOP_ALL_ONES, c.stop);
    for (Fake *f : {&mismatch, &budget, &ones}) {
        EXPECT_TRUE(f->writes().empty());
        EXPECT_EQ(0u, f->first("rail_mark") < f->events.size());
    }
}

TEST(T3, TheRailTimerIsNeverAskedBeforeItIsStarted)
{
    Fake normal;
    normal.run();
    EXPECT_EQ(0u, normal.unmarked_rail_checks);
    Fake mismatch;
    mismatch.reg[T3_PWM_CSR_1] = 1;
    mismatch.run();
    EXPECT_EQ(0u, mismatch.unmarked_rail_checks);
    Fake stopped;
    stopped.refuse_step = SWITCH_T3_STEP_BEGIN(T3_PHASE_RAIL);
    stopped.run();
    EXPECT_EQ(0u, stopped.unmarked_rail_checks);
}

TEST(T3, ThreeEndingsAreToldApart)
{
    Fake before;
    before.reg[T3_PWM_CSR_1] = 1;
    const switch_t3_result unchecked = before.run();
    Fake clean;
    const switch_t3_result off = clean.run();
    Fake stuck;
    stuck.rail_off_ignored = 100;
    const switch_t3_result live = stuck.run();
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_UNCHECKED), unchecked.rail);
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_OFF_CONFIRMED), off.rail);
    EXPECT_EQ(static_cast<unsigned>(T3_RAIL_MAYBE_ON), live.rail);
    EXPECT_EQ(0u, unchecked.writes);
    EXPECT_GT(off.writes, 0u);
    EXPECT_NE(SWITCH_T3_RAIL_NOT_OFF, unchecked.stop);
    EXPECT_NE(SWITCH_T3_RAIL_NOT_OFF, off.stop);
    EXPECT_EQ(SWITCH_T3_RAIL_NOT_OFF, live.stop);
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3_STEP_DONE(T3_PHASE_OFF), off).find("LATCH READ"));
    Fake reads;
    reads.expire_reads_after = 3;
    reads.run();
    EXPECT_NE(std::string::npos, text_of(SWITCH_T3_STEP_DONE(T3_PHASE_STATE), reads.drawn[SWITCH_T3_STEP_DONE(T3_PHASE_STATE)]).find("RAIL NOT CHECKED"));
}
