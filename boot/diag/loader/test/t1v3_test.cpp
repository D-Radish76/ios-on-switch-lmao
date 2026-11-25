// T1 v3 on the host: the allowlist, the run against a model of the I2C1 controller and the TMP451, every stop, and the
// exact sequence of writes.
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "../t1v3.h"
#include "../t1v3_report.h"
}

static std::vector<std::pair<uint32_t, uint32_t>> gate_writes;
static std::vector<uint32_t> gate_reads;

extern "C" {
uint32_t switch_t1v3_read32(uint32_t address)
{
    gate_reads.push_back(address);
    return 0xabcd;
}
void switch_t1v3_write32(uint32_t address, uint32_t value)
{
    gate_writes.emplace_back(address, value);
}
}

namespace {
using Write = std::pair<unsigned, uint32_t>;

constexpr uint32_t kPin0 = 0x00000204u, kPin1 = 0x0a0a0a0au;

bool is_i2c(unsigned reg)
{
    return reg >= T1V3_I2C1_CNFG;
}

enum class ReadFault { none, noack, hang, never_starts, late_busy };

struct Fake {
    bool clock_on = true, in_reset = false, ack = true, frozen = false, divisor_stuck = false;
    bool apply_clears_bit2 = false, id_not_delivered = false, freeze_on_go_poll = false;
    uint32_t source = SWITCH_T1V3_SOURCE_EXPECTED, pllp = 0x48115408, divisor = SWITCH_T1V3_DIVISOR_BEFORE;
    uint32_t timing_0 = kPin0, timing_1 = kPin1, int_word = 0, int_after_load = 0, status_busy = 0;
    uint32_t load_word = SWITCH_T1V3_LOAD_REST;
    /* CONFIG_LOAD number n (1 is the divisor, 2 and 3 the transfers) reads bit 0 set for ever, or reads this word. */
    unsigned load_stuck_from = 0, load_odd_at = 0;
    uint32_t load_odd_word = 0;
    bool busy_after_load = false;
    ReadFault fault = ReadFault::none;
    /* 0 is off. The n-th read of STATUS (1 based) reports BUSY. */
    unsigned busy_on_status_read = 0, status_reads = 0;
    unsigned freeze_at_read = 0;
    int data1_pinned = -1;
    uint8_t id = 0x55;
    uint64_t duration = 400, busy_delay = 0;
    uint64_t now = 0, go_at = 0;
    uint32_t addr0 = 0, data1 = 0, noack_bits = 0;
    uint8_t pointer = 0;
    bool in_flight = false, loaded = false, started = false;
    unsigned i2c_touches = 0, go_writes = 0, reads = 0, loads = 0;
    std::vector<Write> writes;

    bool read_go() const { return in_flight && (addr0 & 1); }
    uint64_t go_duration() const { return read_go() && fault == ReadFault::hang ? UINT64_C(1) << 40 : duration; }
    uint64_t go_busy_delay() const { return read_go() && fault == ReadFault::late_busy ? 5000 : busy_delay; }
    bool done() const { return in_flight && started && now >= go_at + go_duration(); }
    bool busy() const { return in_flight && started && now >= go_at + go_busy_delay() && now < go_at + go_duration(); }
    uint32_t read(unsigned reg)
    {
        ++reads;
        if (freeze_at_read && reads >= freeze_at_read)
            frozen = true;
        if (!frozen)
            now += 5;
        if (is_i2c(reg))
            ++i2c_touches;
        switch (reg) {
        case T1V3_CAR_RST_DEVICES_L: return in_reset ? (1u << 12) : 0;
        case T1V3_CAR_CLK_OUT_ENB_L: return clock_on ? (1u << 12) : 0;
        case T1V3_CAR_CLK_SOURCE_I2C1: return source;
        case T1V3_CAR_PLLP_BASE: return pllp;
        case T1V3_I2C1_CNFG: return 0x2842;
        case T1V3_I2C1_INT_STATUS: return loaded ? int_after_load : int_word;
        case T1V3_I2C1_CLK_DIVISOR: return divisor;
        case T1V3_I2C1_TIMING_0: return timing_0;
        case T1V3_I2C1_TIMING_1: return timing_1;
        case T1V3_I2C1_STATUS: {
            if (in_flight && freeze_on_go_poll)
                frozen = true;
            const bool forced = ++status_reads == busy_on_status_read || (loaded && busy_after_load && !in_flight);
            return (busy() || forced ? 0x100 : 0) | noack_bits | status_busy;
        }
        case T1V3_I2C1_CONFIG_LOAD:
            if (load_stuck_from && loads >= load_stuck_from)
                return load_word | 1;
            return load_word;
        case T1V3_I2C1_CMD_DATA1:
            if (data1_pinned >= 0)
                return static_cast<uint32_t>(data1_pinned);
            if (done() && (addr0 & 1) && !id_not_delivered)
                data1 = id;
            return data1;
        default: return 0;
        }
    }
    void write(unsigned reg, uint32_t v)
    {
        if (!frozen)
            now += 5;
        writes.emplace_back(reg, v);
        if (is_i2c(reg))
            ++i2c_touches;
        if (reg == T1V3_I2C1_CLK_DIVISOR && !divisor_stuck)
            divisor = v;
        else if (reg == T1V3_I2C1_CONFIG_LOAD) {
            ++loads;
            if (v == SWITCH_T1V3_LOAD_APPLY) {
                loaded = true;
                load_word = apply_clears_bit2 ? 0 : SWITCH_T1V3_LOAD_REST;
            } else {
                load_word = SWITCH_T1V3_LOAD_REST;
            }
            if (load_odd_at && loads == load_odd_at)
                load_word = load_odd_word;
        } else if (reg == T1V3_I2C1_CMD_ADDR0)
            addr0 = v;
        else if (reg == T1V3_I2C1_CMD_DATA1)
            data1 = v;
        else if (reg == T1V3_I2C1_CNFG && (v & 0x200)) {
            ++go_writes;
            go_at = now;
            in_flight = true;
            started = !(read_go() && fault == ReadFault::never_starts);
            noack_bits = (!ack || (read_go() && fault == ReadFault::noack)) ? 1 : 0;
            if (!(addr0 & 1))
                pointer = static_cast<uint8_t>(data1);
        }
    }
};

uint32_t f_read(void *c, unsigned r) { return static_cast<Fake *>(c)->read(r); }
void f_write(void *c, unsigned r, uint32_t v) { static_cast<Fake *>(c)->write(r, v); }
uint64_t f_now(void *c)
{
    auto *f = static_cast<Fake *>(c);
    if (!f->frozen)
        f->now += 1;
    return f->now;
}

switch_t1v3_result run(Fake &fake, uint32_t pin0 = kPin0, uint32_t pin1 = kPin1)
{
    const switch_t1v3_io io = {&fake, pin0, pin1, f_read, f_write, f_now};
    switch_t1v3_result result;
    switch_t1v3_run(&io, &result);
    return result;
}

std::vector<Write> full_sequence()
{
    return {
        {T1V3_I2C1_CLK_DIVISOR, 0x00ff0001}, {T1V3_I2C1_CONFIG_LOAD, 0x01},   {T1V3_I2C1_CMD_ADDR0, 0x98},
        {T1V3_I2C1_CMD_DATA1, 0xfe},         {T1V3_I2C1_CNFG, 0x2800},        {T1V3_I2C1_CONFIG_LOAD, 0x25},
        {T1V3_I2C1_CNFG, 0x2a00},            {T1V3_I2C1_CMD_ADDR0, 0x99},     {T1V3_I2C1_CNFG, 0x2840},
        {T1V3_I2C1_CONFIG_LOAD, 0x25},       {T1V3_I2C1_CNFG, 0x2a40},
    };
}

std::vector<Write> prefix(size_t count)
{
    const auto all = full_sequence();
    return {all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count)};
}

bool no_car_write(const Fake &fake)
{
    for (const auto &w : fake.writes)
        if (!is_i2c(w.first))
            return false;
    return true;
}

std::string text(const switch_t1v3_line &line) { return line.text.text; }
}  // namespace

TEST(T1v3Allowlist, ReadsAreTheListedRegistersOnly)
{
    for (unsigned reg = 0; reg < T1V3_REG_COUNT + 4; ++reg) {
        const bool readable = reg < T1V3_REG_COUNT && reg != T1V3_I2C1_CMD_ADDR0;
        EXPECT_EQ(switch_t1v3_may_read(reg), readable ? 1 : 0);
    }
}

TEST(T1v3Allowlist, WritesAreTheListedValuesOfTheListedRegisters)
{
    const std::set<Write> allowed = {
        {T1V3_I2C1_CMD_ADDR0, 0x98}, {T1V3_I2C1_CMD_ADDR0, 0x99},   {T1V3_I2C1_CMD_DATA1, 0xfe},
        {T1V3_I2C1_CNFG, 0x2800},    {T1V3_I2C1_CNFG, 0x2840},      {T1V3_I2C1_CNFG, 0x2a00},
        {T1V3_I2C1_CNFG, 0x2a40},    {T1V3_I2C1_CLK_DIVISOR, 0x00ff0001}, {T1V3_I2C1_CONFIG_LOAD, 0x01},
        {T1V3_I2C1_CONFIG_LOAD, 0x25},
    };
    for (unsigned reg = 0; reg < T1V3_REG_COUNT + 2; ++reg) {
        for (uint32_t value = 0; value < 0x3000; ++value)
            EXPECT_EQ(switch_t1v3_may_write(reg, value), allowed.count({reg, value}) ? 1 : 0);
        for (uint32_t value : {0x00ff0001u, 0x00050001u, 0x007f0001u, 0x00fe0001u, 0x00ff0000u, 0xffffffffu})
            EXPECT_EQ(switch_t1v3_may_write(reg, value), allowed.count({reg, value}) ? 1 : 0);
    }
}

TEST(T1v3Allowlist, NothingIsWritableOutsideI2c1OrInTheReadOnlyI2cRegisters)
{
    for (unsigned reg : {unsigned(T1V3_CAR_RST_DEVICES_L), unsigned(T1V3_CAR_CLK_OUT_ENB_L),
                         unsigned(T1V3_CAR_CLK_SOURCE_I2C1), unsigned(T1V3_CAR_PLLP_BASE), unsigned(T1V3_I2C1_STATUS),
                         unsigned(T1V3_I2C1_INT_STATUS), unsigned(T1V3_I2C1_TIMING_0), unsigned(T1V3_I2C1_TIMING_1)})
        for (uint32_t value : {0u, 1u, 3u, 0x25u, 0x2800u, 0x98u, 0xfeu, 0x00ff0001u, 0xffffffffu})
            EXPECT_EQ(switch_t1v3_may_write(reg, value), 0);
}

TEST(T1v3Allowlist, AddressesAreTheReviewedOnes)
{
    EXPECT_EQ(switch_t1v3_address(T1V3_CAR_RST_DEVICES_L), 0x60006004u);
    EXPECT_EQ(switch_t1v3_address(T1V3_CAR_CLK_OUT_ENB_L), 0x60006010u);
    EXPECT_EQ(switch_t1v3_address(T1V3_CAR_CLK_SOURCE_I2C1), 0x60006124u);
    EXPECT_EQ(switch_t1v3_address(T1V3_CAR_PLLP_BASE), 0x600060a0u);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_CNFG), 0x7000c000u);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_CMD_ADDR0), 0x7000c004u);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_CMD_DATA1), 0x7000c00cu);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_STATUS), 0x7000c01cu);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_INT_STATUS), 0x7000c068u);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_CLK_DIVISOR), 0x7000c06cu);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_CONFIG_LOAD), 0x7000c08cu);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_TIMING_0), 0x7000c094u);
    EXPECT_EQ(switch_t1v3_address(T1V3_I2C1_TIMING_1), 0x7000c098u);
    EXPECT_EQ(switch_t1v3_address(T1V3_REG_COUNT), 0u);
}

TEST(T1v3Gate, RefusedAccessesNeverReachTheDevice)
{
    gate_writes.clear();
    gate_reads.clear();
    switch_t1v3_access(T1V3_I2C1_CLK_DIVISOR, 1, 0x00050001);
    switch_t1v3_access(T1V3_I2C1_CLK_DIVISOR, 1, 0x007f0001);
    switch_t1v3_access(T1V3_CAR_CLK_OUT_ENB_L, 1, 0);
    switch_t1v3_access(T1V3_CAR_PLLP_BASE, 1, 0);
    switch_t1v3_access(T1V3_I2C1_CNFG, 1, 0x2801);
    switch_t1v3_access(T1V3_I2C1_TIMING_0, 1, 0x204);
    switch_t1v3_access(T1V3_I2C1_CMD_ADDR0, 0, 0);
    switch_t1v3_access(T1V3_REG_COUNT, 0, 0);
    switch_t1v3_access(1000, 1, 0x98);
    EXPECT_TRUE(gate_writes.empty());
    EXPECT_TRUE(gate_reads.empty());
}

TEST(T1v3Gate, ListedAccessesReachTheReviewedAddress)
{
    gate_writes.clear();
    gate_reads.clear();
    switch_t1v3_access(T1V3_I2C1_CLK_DIVISOR, 1, 0x00ff0001);
    EXPECT_EQ(switch_t1v3_access(T1V3_I2C1_TIMING_0, 0, 0), 0xabcdu);
    ASSERT_EQ(gate_writes.size(), 1u);
    EXPECT_EQ(gate_writes[0], std::make_pair(0x7000c06cu, 0x00ff0001u));
    ASSERT_EQ(gate_reads.size(), 1u);
    EXPECT_EQ(gate_reads[0], 0x7000c094u);
}

TEST(T1v3Run, NormalRunWritesTheElevenReviewedWordsInOrder)
{
    Fake fake;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, 0);
    EXPECT_EQ(fake.writes, full_sequence());
    EXPECT_EQ(r.writes, 11u);
    EXPECT_EQ(r.transfers, 2u);
    EXPECT_EQ(r.divisor, 0x00050001u);
    EXPECT_EQ(r.divisor_after, 0x00ff0001u);
    EXPECT_EQ(fake.divisor, 0x00ff0001u);
    EXPECT_EQ(r.pllp_base, 0x48115408u);
    EXPECT_EQ(r.timing_0, kPin0);
    EXPECT_EQ(r.timing_1, kPin1);
    EXPECT_EQ(r.load_after, 4u);
    EXPECT_EQ(r.load_transfer[0], 4u);
    EXPECT_EQ(r.load_transfer[1], 4u);
    EXPECT_EQ(r.data_before_read, 0xfeu);
    EXPECT_TRUE(r.have_id);
    EXPECT_EQ(r.id, 0x55);
    EXPECT_TRUE(no_car_write(fake));
    EXPECT_EQ(fake.go_writes, 2u);
}

TEST(T1v3Run, ClearingBit2OnTheApplyIsAcceptedAndTheTransferLoadsSetItAgain)
{
    Fake fake;
    fake.apply_clears_bit2 = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, 0);
    EXPECT_EQ(r.load_after, 0u);
    EXPECT_EQ(r.load_transfer[0], 4u);
    EXPECT_EQ(fake.writes, full_sequence());
}

TEST(T1v3Run, TheDivisorIsWrittenOnceAndTheIdIsReadOnce)
{
    Fake fake;
    run(fake);
    unsigned divisor_writes = 0, pointer_writes = 0;
    for (const auto &w : fake.writes) {
        divisor_writes += w.first == T1V3_I2C1_CLK_DIVISOR;
        pointer_writes += w.first == T1V3_I2C1_CMD_DATA1;
    }
    EXPECT_EQ(divisor_writes, 1u);
    EXPECT_EQ(pointer_writes, 1u);
    EXPECT_EQ(fake.divisor, 0x00ff0001u);
}

TEST(T1v3Run, StartAndClearTimesAreRecordedAsAReference)
{
    Fake fake;
    const auto r = run(fake);
    for (const auto &t : r.transaction) {
        EXPECT_EQ(t.started, 1);
        EXPECT_LT(t.started_us, 100u);
        EXPECT_GE(t.elapsed_us, 400u);
        EXPECT_LT(t.elapsed_us, 600u);
        EXPECT_EQ(t.cnfg, 0x2842u);
    }
}

TEST(T1v3Stops, ClockOffOrInResetTouchesNoI2c1RegisterAndWritesNothing)
{
    {
        Fake fake;
        fake.clock_on = false;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_I2C_CLOCK_OFF);
        EXPECT_EQ(fake.i2c_touches, 0u);
        EXPECT_TRUE(fake.writes.empty());
    }
    {
        Fake fake;
        fake.in_reset = true;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_I2C_IN_RESET);
        EXPECT_EQ(fake.i2c_touches, 0u);
        EXPECT_TRUE(fake.writes.empty());
    }
}

TEST(T1v3Stops, SourceAndPllpMismatchTouchNoI2c1RegisterAndWriteNothing)
{
    {
        Fake fake;
        fake.source = 0xc0000003;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_SOURCE_UNEXPECTED);
        EXPECT_EQ(fake.i2c_touches, 0u);
        EXPECT_TRUE(fake.writes.empty());
    }
    for (uint32_t pllp : {0x48115409u, 0x48115008u, 0x48115400u, 0x48215408u, 0x48015408u, 0u}) {
        Fake fake;
        fake.pllp = pllp;
        const auto r = run(fake);
        EXPECT_EQ(r.stop, SWITCH_T1V3_PLLP_UNEXPECTED);
        EXPECT_EQ(r.pllp_base, pllp);
        EXPECT_EQ(fake.i2c_touches, 0u);
        EXPECT_TRUE(fake.writes.empty());
    }
}

TEST(T1v3Stops, ThePllpGateReadsTheFieldsNotTheWholeWord)
{
    for (uint32_t pllp : {0x48115408u, 0xc8115408u, 0x00115408u, 0x48115409u}) {
        Fake fake;
        fake.pllp = pllp;
        const auto r = run(fake);
        EXPECT_EQ(r.stop == 0, SWITCH_T1V3_PLLP_M(pllp) == 8 && SWITCH_T1V3_PLLP_N(pllp) == 85 && SWITCH_T1V3_PLLP_P(pllp) == 1);
    }
}

TEST(T1v3Stops, GateMismatchWritesNothing)
{
    {
        Fake fake;
        fake.status_busy = 0x100;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_BUS_BUSY);
        EXPECT_TRUE(fake.writes.empty());
    }
    {
        Fake fake;
        fake.int_word = 1;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_INT_PENDING);
        EXPECT_TRUE(fake.writes.empty());
    }
    for (uint32_t divisor : {0x00000000u, 0x00ff0001u, 0x007f0001u, 0x00050000u, 0x00060001u}) {
        Fake fake;
        fake.divisor = divisor;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_DIVISOR_UNEXPECTED);
        EXPECT_TRUE(fake.writes.empty());
    }
}

TEST(T1v3Stops, ConfigLoadMustReadItsRestWordBeforeTheDivisorWrite)
{
    for (uint32_t word : {0x00000000u, 0x00000024u, 0x00000020u, 0x00000104u, 0xffffffffu}) {
        Fake fake;
        fake.load_word = word;
        const auto r = run(fake);
        EXPECT_EQ(r.stop, word & 1 ? SWITCH_T1V3_LOAD_BEFORE_PENDING : SWITCH_T1V3_LOAD_BEFORE_UNEXPECTED);
        EXPECT_EQ(r.config_load, word);
        EXPECT_TRUE(fake.writes.empty());
    }
    for (uint32_t word : {0x00000001u, 0x00000005u, 0x00000025u}) {
        Fake fake;
        fake.load_word = word;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_LOAD_BEFORE_PENDING);
        EXPECT_TRUE(fake.writes.empty());
    }
}

TEST(T1v3Stops, AbnormalTimingRegistersStopBeforeTheFirstWrite)
{
    struct Case { uint32_t t0, t1; int stop; };
    const Case cases[] = {
        {0, kPin1, SWITCH_T1V3_TIMING_ZERO},
        {kPin0, 0, SWITCH_T1V3_TIMING_ZERO},
        {0xffffffff, kPin1, SWITCH_T1V3_TIMING_UNREADABLE},
        {kPin0, 0xffffffff, SWITCH_T1V3_TIMING_UNREADABLE},
        {0x205, kPin1, SWITCH_T1V3_TIMING_UNEXPECTED},
        {0x00040204, kPin1, SWITCH_T1V3_TIMING_UNEXPECTED},
        {kPin0, 0x0a0a0a0b, SWITCH_T1V3_TIMING_UNEXPECTED},
        {kPin0, 0x04070404, SWITCH_T1V3_TIMING_UNEXPECTED},
    };
    for (const Case &c : cases) {
        Fake fake;
        fake.timing_0 = c.t0;
        fake.timing_1 = c.t1;
        const auto r = run(fake);
        EXPECT_EQ(r.stop, c.stop);
        EXPECT_TRUE(fake.writes.empty());
        EXPECT_EQ(r.writes, 0u);
    }
}

TEST(T1v3Stops, WithoutAReviewedValueTheRunReadsAndStopsBeforeTheFirstWrite)
{
    for (auto pins : {std::make_pair(0u, 0u), std::make_pair(0u, kPin1), std::make_pair(kPin0, 0u)}) {
        Fake fake;
        fake.timing_0 = 0x204;
        fake.timing_1 = 0x04070404;
        const auto r = run(fake, pins.first, pins.second);
        EXPECT_EQ(r.stop, SWITCH_T1V3_TIMING_NOT_PINNED);
        EXPECT_EQ(r.timing_0, 0x204u);
        EXPECT_EQ(r.timing_1, 0x04070404u);
        EXPECT_TRUE(fake.writes.empty());
        EXPECT_EQ(r.writes, 0u);
    }
}

TEST(T1v3Stops, BusIdleNotSatisfiedWritesNothing)
{
    for (unsigned sample = 2; sample <= 4; ++sample) {
        Fake fake;
        fake.busy_on_status_read = sample;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_IDLE_FAILED);
        EXPECT_TRUE(fake.writes.empty());
    }
}

TEST(T1v3Stops, StoppedCounterAtTheStartTouchesNothing)
{
    Fake fake;
    fake.frozen = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_COUNTER_STALLED);
    EXPECT_EQ(fake.reads, 0u);
    EXPECT_TRUE(fake.writes.empty());
}

TEST(T1v3Stops, CounterStoppingInsideTheIdleWaitWritesNothing)
{
    Fake fake;
    fake.freeze_at_read = 13;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_COUNTER_STALLED);
    EXPECT_TRUE(fake.writes.empty());
}

TEST(T1v3Stops, ConfigLoadWaitEndsByCountWhenTheCounterIsStopped)
{
    Fake fake;
    fake.load_stuck_from = 1;
    fake.freeze_at_read = 20;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_CONFIG_LOAD_TIMEOUT);
    EXPECT_EQ(fake.writes, prefix(2));
}

TEST(T1v3Stops, TheStartWindowAndTheBusyWaitEndByCountWhenTheCounterStops)
{
    {
        Fake fake;
        fake.fault = ReadFault::none;
        fake.freeze_on_go_poll = true;
        fake.duration = UINT64_C(1) << 40;
        const auto r = run(fake);
        EXPECT_EQ(r.stop, SWITCH_T1V3_BUSY_TIMEOUT);
        EXPECT_EQ(fake.writes, prefix(7));
    }
    {
        Fake fake;
        fake.freeze_on_go_poll = true;
        fake.busy_delay = 5000;
        const auto r = run(fake);
        EXPECT_EQ(r.stop, SWITCH_T1V3_NOT_STARTED);
        EXPECT_EQ(fake.writes, prefix(7));
    }
}

struct Scenario {
    const char *name;
    void (*configure)(Fake &);
    int stop;
    size_t writes;
    unsigned go_writes;
    unsigned transfers;
};

/* Every stop that can happen after the first write, and the exact words written up to it. A stop is detected only after a
 * load poll (writes 2, 6 and 10) or a go (writes 7 and 11); the writes between them are made without a check in between,
 * and put() refuses anything once a stop code exists. */
const Scenario scenarios[] = {
    {"apply: load never finishes", [](Fake &f) { f.load_stuck_from = 1; }, SWITCH_T1V3_CONFIG_LOAD_TIMEOUT, 2, 0, 0},
    {"apply: divisor readback differs", [](Fake &f) { f.divisor_stuck = true; }, SWITCH_T1V3_DIVISOR_READBACK, 2, 0, 0},
    {"apply: load word 0x20", [](Fake &f) { f.load_odd_at = 1; f.load_odd_word = 0x20; }, SWITCH_T1V3_LOAD_AFTER_UNEXPECTED, 2, 0, 0},
    {"apply: load word 0x24", [](Fake &f) { f.load_odd_at = 1; f.load_odd_word = 0x24; }, SWITCH_T1V3_LOAD_AFTER_UNEXPECTED, 2, 0, 0},
    {"apply: load word 0x104", [](Fake &f) { f.load_odd_at = 1; f.load_odd_word = 0x104; }, SWITCH_T1V3_LOAD_AFTER_UNEXPECTED, 2, 0, 0},
    {"apply: busy after", [](Fake &f) { f.busy_after_load = true; }, SWITCH_T1V3_NOT_IDLE, 2, 0, 0},
    {"apply: interrupt after", [](Fake &f) { f.int_after_load = 2; }, SWITCH_T1V3_INT_CHANGED, 2, 0, 0},
    {"first load never finishes", [](Fake &f) { f.load_stuck_from = 2; }, SWITCH_T1V3_CONFIG_LOAD_TIMEOUT, 6, 0, 0},
    {"first load word unexpected", [](Fake &f) { f.load_odd_at = 2; f.load_odd_word = 0x24; }, SWITCH_T1V3_LOAD_AFTER_UNEXPECTED, 6, 0, 0},
    {"first load word 0", [](Fake &f) { f.load_odd_at = 2; f.load_odd_word = 0; }, SWITCH_T1V3_LOAD_AFTER_UNEXPECTED, 6, 0, 0},
    {"pointer write: no ack", [](Fake &f) { f.ack = false; }, SWITCH_T1V3_NOACK, 7, 1, 0},
    {"pointer write: busy never clears", [](Fake &f) { f.duration = UINT64_C(1) << 40; }, SWITCH_T1V3_BUSY_TIMEOUT, 7, 1, 0},
    {"pointer write: busy never rises", [](Fake &f) { f.busy_delay = 5000; }, SWITCH_T1V3_NOT_STARTED, 7, 1, 0},
    {"data1 already holds 55", [](Fake &f) { f.data1_pinned = 0x55; }, SWITCH_T1V3_STALE_DATA, 7, 1, 1},
    {"second load never finishes", [](Fake &f) { f.load_stuck_from = 3; }, SWITCH_T1V3_CONFIG_LOAD_TIMEOUT, 10, 1, 1},
    {"second load word unexpected", [](Fake &f) { f.load_odd_at = 3; f.load_odd_word = 0x20; }, SWITCH_T1V3_LOAD_AFTER_UNEXPECTED, 10, 1, 1},
    {"read: no ack", [](Fake &f) { f.fault = ReadFault::noack; }, SWITCH_T1V3_NOACK, 11, 2, 1},
    {"read: busy never clears", [](Fake &f) { f.fault = ReadFault::hang; }, SWITCH_T1V3_BUSY_TIMEOUT, 11, 2, 1},
    {"read: never starts", [](Fake &f) { f.fault = ReadFault::never_starts; }, SWITCH_T1V3_NOT_STARTED, 11, 2, 1},
    {"read: busy rises after the window", [](Fake &f) { f.fault = ReadFault::late_busy; }, SWITCH_T1V3_NOT_STARTED, 11, 2, 1},
    {"read: wrong id", [](Fake &f) { f.id = 0x12; }, SWITCH_T1V3_ID_MISMATCH, 11, 2, 2},
    {"read: id not delivered", [](Fake &f) { f.id_not_delivered = true; }, SWITCH_T1V3_ID_MISMATCH, 11, 2, 2},
};

TEST(T1v3Stops, EveryStopAfterTheFirstWriteEndsTheWritesAtItsOwnPointWithListedWordsOnly)
{
    for (const Scenario &s : scenarios) {
        Fake fake;
        s.configure(fake);
        const auto r = run(fake);
        EXPECT_EQ(r.stop, s.stop) << s.name;
        EXPECT_EQ(fake.writes, prefix(s.writes)) << s.name;
        EXPECT_EQ(r.writes, s.writes) << s.name;
        EXPECT_EQ(fake.go_writes, s.go_writes) << s.name;
        EXPECT_EQ(r.transfers, s.transfers) << s.name;
        EXPECT_TRUE(no_car_write(fake)) << s.name;
    }
}

TEST(T1v3Stops, EveryWriteCountThatCanEndARunIsCovered)
{
    std::set<size_t> counts;
    for (const Scenario &s : scenarios)
        counts.insert(s.writes);
    EXPECT_EQ(counts, (std::set<size_t>{2, 6, 7, 10, 11}));
}

TEST(T1v3Stops, ARefusedPointerAnswerNeverCountsAsTheId)
{
    for (const Scenario &s : scenarios) {
        Fake fake;
        s.configure(fake);
        const auto r = run(fake);
        if (r.stop != SWITCH_T1V3_ID_MISMATCH)
            EXPECT_FALSE(r.have_id) << s.name;
    }
}

TEST(T1v3Stops, ATransactionWithoutBusyIsNeverTakenAsDone)
{
    Fake fake;
    fake.fault = ReadFault::never_starts;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_NOT_STARTED);
    EXPECT_EQ(r.transaction[1].started, 0);
    EXPECT_EQ(r.transfers, 1u);
    EXPECT_FALSE(r.have_id);
}

TEST(T1v3Stops, AStaleDataByteEqualToTheIdIsNotUsed)
{
    Fake fake;
    fake.data1_pinned = 0x55;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_STALE_DATA);
    EXPECT_EQ(r.data_before_read, 0x55u);
    EXPECT_FALSE(r.have_id);
    EXPECT_EQ(fake.go_writes, 1u);
}

TEST(T1v3Report, ResultLinesNameTheStopTheWordsAndTheReferenceTime)
{
    Fake fake;
    const auto r = run(fake);
    switch_t1v3_line lines[SWITCH_T1V3_LINES];
    switch_t1v3_lines(&r, lines);
    std::string all;
    for (const auto &line : lines)
        all += text(line) + "\n";
    EXPECT_NE(all.find("00FF0001"), std::string::npos);
    EXPECT_NE(all.find("00050001"), std::string::npos);
    EXPECT_NE(all.find("PLLP_BASE 48115408 M 8 N 85 P 1"), std::string::npos);
    EXPECT_NE(all.find("TIMING_0 00000204"), std::string::npos);
    EXPECT_NE(all.find("NOT RESTORED"), std::string::npos);
    EXPECT_NE(all.find("NOT AN SCL MEASUREMENT"), std::string::npos);
    EXPECT_NE(all.find("LOAD AFTER 0X25 00000004 AND 00000004"), std::string::npos);
    EXPECT_NE(all.find("T1V3 COMPLETE CODE 0000"), std::string::npos);
    EXPECT_NE(all.find("WRITES 11"), std::string::npos);
    EXPECT_NE(all.find("ID(FE) 55"), std::string::npos);
    EXPECT_EQ(all.find("DEG"), std::string::npos);
}

TEST(T1v3Report, NoLineIsClipped)
{
    for (const Scenario &s : scenarios) {
        Fake fake;
        s.configure(fake);
        const auto r = run(fake);
        switch_t1v3_line lines[SWITCH_T1V3_LINES];
        switch_t1v3_lines(&r, lines);
        for (const auto &line : lines)
            EXPECT_LT(line.text.length, unsigned(SWITCH_DIAG_LINE_MAX - 1)) << s.name << ": " << text(line);
    }
}

TEST(T1v3Report, EveryStopHasItsOwnName)
{
    std::set<std::string> names;
    for (int stop = SWITCH_T1V3_I2C_CLOCK_OFF; stop <= SWITCH_T1V3_PLLP_UNEXPECTED; ++stop) {
        const std::string name = switch_t1v3_stop_name(stop);
        EXPECT_NE(name, "UNKNOWN");
        EXPECT_TRUE(names.insert(name).second) << name;
        EXPECT_LT(name.size(), 70u) << name;
    }
    EXPECT_EQ(std::string(switch_t1v3_stop_name(0)), "COMPLETE");
}

TEST(T1v3Report, AnUnpinnedRunShowsTheTimingWordsAndAStopLine)
{
    Fake fake;
    fake.timing_1 = 0x04070404;
    const auto r = run(fake, 0, 0);
    switch_t1v3_line lines[SWITCH_T1V3_LINES];
    switch_t1v3_lines(&r, lines);
    EXPECT_NE(text(lines[3]).find("TIMING_1 04070404"), std::string::npos);
    EXPECT_NE(text(lines[10]).find("T1V3 STOPPED CODE 061A"), std::string::npos);
    EXPECT_EQ(lines[10].bad, 1);
}

namespace {
Fake measured_fake()
{
    Fake fake;
    fake.timing_0 = SWITCH_T1V3_TIMING_0_PIN;
    fake.timing_1 = SWITCH_T1V3_TIMING_1_PIN;
    return fake;
}

switch_t1v3_result run_production_pins(Fake &fake)
{
    return run(fake, SWITCH_T1V3_TIMING_0_PIN, SWITCH_T1V3_TIMING_1_PIN);
}
} // namespace

TEST(T1v3Pins, TheProductionPinsAreTheWordsT1TMeasured)
{
    EXPECT_EQ(SWITCH_T1V3_TIMING_0_PIN, 0x00000204u);
    EXPECT_EQ(SWITCH_T1V3_TIMING_1_PIN, 0x04070404u);
    EXPECT_EQ(SWITCH_T1V3_DIVISOR_AFTER, 0x00ff0001u);
}

TEST(T1v3Pins, TheMeasuredWordsWithTheProductionPinsTakeTheNormalPath)
{
    Fake fake = measured_fake();
    const auto r = run_production_pins(fake);
    EXPECT_EQ(r.stop, 0);
    EXPECT_EQ(fake.writes, full_sequence());
    EXPECT_EQ(r.timing_0, SWITCH_T1V3_TIMING_0_PIN);
    EXPECT_EQ(r.timing_1, SWITCH_T1V3_TIMING_1_PIN);
    EXPECT_TRUE(no_car_write(fake));
}

TEST(T1v3Pins, EveryWriteOfTheNormalPathIsOneOfTheFiveI2c1Registers)
{
    Fake fake = measured_fake();
    run_production_pins(fake);
    for (const Write &w : fake.writes) {
        const unsigned reg = w.first;
        EXPECT_TRUE(reg == T1V3_I2C1_CMD_ADDR0 || reg == T1V3_I2C1_CMD_DATA1 || reg == T1V3_I2C1_CNFG ||
                    reg == T1V3_I2C1_CLK_DIVISOR || reg == T1V3_I2C1_CONFIG_LOAD);
    }
}

TEST(T1v3Pins, AOneBitDifferenceInEitherWordWritesNothing)
{
    for (unsigned bit = 0; bit < 32; ++bit) {
        for (unsigned which = 0; which < 2; ++which) {
            Fake fake = measured_fake();
            if (which == 0)
                fake.timing_0 ^= 1u << bit;
            else
                fake.timing_1 ^= 1u << bit;
            const auto r = run_production_pins(fake);
            EXPECT_EQ(r.stop, SWITCH_T1V3_TIMING_UNEXPECTED);
            EXPECT_TRUE(fake.writes.empty());
            EXPECT_EQ(r.writes, 0u);
        }
    }
}

TEST(T1v3Pins, ABitFlippedInThePinInsteadOfTheRegisterWritesNothingEither)
{
    for (unsigned bit = 0; bit < 32; ++bit) {
        Fake fake = measured_fake();
        EXPECT_EQ(run(fake, SWITCH_T1V3_TIMING_0_PIN ^ (1u << bit), SWITCH_T1V3_TIMING_1_PIN).stop,
                  SWITCH_T1V3_TIMING_UNEXPECTED);
        EXPECT_TRUE(fake.writes.empty());
    }
}
