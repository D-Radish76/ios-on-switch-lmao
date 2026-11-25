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

bool is_i2c(unsigned reg)
{
    return reg >= T1V3_I2C1_CNFG;
}

struct Fake {
    bool clock_on = true, in_reset = false, ack = true, load_stuck = false, frozen = false, divisor_stuck = false;
    bool freeze_on_busy_poll = false;
    uint32_t source = SWITCH_T1V3_SOURCE_EXPECTED, divisor = SWITCH_T1V3_DIVISOR_BEFORE;
    uint32_t timing_0 = SWITCH_T1V3_TIMING_0, timing_1 = 0x0a0a0a0a, int_word = 0, int_after_load = 0;
    bool busy_after_load = false;
    uint32_t status_busy = 0;
    /* The n-th read of STATUS (1 based) reports BUSY. 0 is never. */
    unsigned busy_on_status_read = 0, status_reads = 0;
    /* Freezes the counter once this many register reads have happened. 0 is never. */
    unsigned freeze_at_read = 0;
    uint8_t id = 0x55;
    uint64_t duration = 400;
    uint64_t now = 0, go_at = 0;
    uint32_t addr0 = 0, data1 = 0, noack_bits = 0;
    uint8_t pointer = 0;
    bool in_flight = false, loaded = false;
    unsigned i2c_touches = 0, go_writes = 0, reads = 0;
    std::vector<Write> writes;

    bool done() const { return in_flight && now >= go_at + duration; }
    bool busy() const { return in_flight && now < go_at + duration; }
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
        case T1V3_I2C1_CNFG: return 0x2842;
        case T1V3_I2C1_INT_STATUS: return loaded ? int_after_load : int_word;
        case T1V3_I2C1_CLK_DIVISOR: return divisor;
        case T1V3_I2C1_TIMING_0: return timing_0;
        case T1V3_I2C1_TIMING_1: return timing_1;
        case T1V3_I2C1_STATUS: {
            const bool poll = in_flight && now >= go_at + SWITCH_T1V3_SETTLE_US;
            if (poll && freeze_on_busy_poll)
                frozen = true;
            const bool forced = ++status_reads == busy_on_status_read || (loaded && busy_after_load && !in_flight);
            return (busy() || forced ? 0x100 : 0) | noack_bits | status_busy;
        }
        case T1V3_I2C1_CONFIG_LOAD: return load_stuck ? 5 : 4;
        case T1V3_I2C1_CMD_DATA1:
            if (done() && (addr0 & 1))
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
        else if (reg == T1V3_I2C1_CONFIG_LOAD && v == SWITCH_T1V3_LOAD_APPLY)
            loaded = true;
        else if (reg == T1V3_I2C1_CMD_ADDR0)
            addr0 = v;
        else if (reg == T1V3_I2C1_CMD_DATA1)
            data1 = v;
        else if (reg == T1V3_I2C1_CNFG && (v & 0x200)) {
            ++go_writes;
            go_at = now;
            in_flight = true;
            noack_bits = ack ? 0 : 1;
            if (ack && !(addr0 & 1))
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

switch_t1v3_result run(Fake &fake)
{
    const switch_t1v3_io io = {&fake, f_read, f_write, f_now};
    switch_t1v3_result result;
    switch_t1v3_run(&io, &result);
    return result;
}

const std::vector<Write> divisor_and_load = {
    {T1V3_I2C1_CLK_DIVISOR, SWITCH_T1V3_DIVISOR_AFTER},
    {T1V3_I2C1_CONFIG_LOAD, SWITCH_T1V3_LOAD_APPLY},
};

std::vector<Write> full_sequence()
{
    return {
        {T1V3_I2C1_CLK_DIVISOR, 0x00ff0001}, {T1V3_I2C1_CONFIG_LOAD, 0x01},   {T1V3_I2C1_CMD_ADDR0, 0x98},
        {T1V3_I2C1_CMD_DATA1, 0xfe},         {T1V3_I2C1_CNFG, 0x2800},        {T1V3_I2C1_CONFIG_LOAD, 0x25},
        {T1V3_I2C1_CNFG, 0x2a00},            {T1V3_I2C1_CMD_ADDR0, 0x99},     {T1V3_I2C1_CNFG, 0x2840},
        {T1V3_I2C1_CONFIG_LOAD, 0x25},       {T1V3_I2C1_CNFG, 0x2a40},
    };
}

bool no_car_write(const Fake &fake)
{
    for (const auto &w : fake.writes)
        if (!is_i2c(w.first))
            return false;
    return true;
}

/* Everything the run did on an anomaly was a write the allowlist lists, and no CAR register was written. */
void expect_only_listed_writes(const Fake &fake)
{
    for (const auto &w : fake.writes)
        EXPECT_EQ(switch_t1v3_may_write(w.first, w.second), 1);
    EXPECT_TRUE(no_car_write(fake));
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
                         unsigned(T1V3_CAR_CLK_SOURCE_I2C1), unsigned(T1V3_I2C1_STATUS), unsigned(T1V3_I2C1_INT_STATUS),
                         unsigned(T1V3_I2C1_TIMING_0), unsigned(T1V3_I2C1_TIMING_1)})
        for (uint32_t value : {0u, 1u, 3u, 0x25u, 0x2800u, 0x98u, 0xfeu, 0x00ff0001u, 0xffffffffu})
            EXPECT_EQ(switch_t1v3_may_write(reg, value), 0);
}

TEST(T1v3Allowlist, AddressesAreTheReviewedOnes)
{
    EXPECT_EQ(switch_t1v3_address(T1V3_CAR_RST_DEVICES_L), 0x60006004u);
    EXPECT_EQ(switch_t1v3_address(T1V3_CAR_CLK_OUT_ENB_L), 0x60006010u);
    EXPECT_EQ(switch_t1v3_address(T1V3_CAR_CLK_SOURCE_I2C1), 0x60006124u);
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
    EXPECT_EQ(r.timing_0, 0x204u);
    EXPECT_EQ(r.timing_1, 0x0a0a0a0au);
    EXPECT_TRUE(r.have_id);
    EXPECT_EQ(r.id, 0x55);
    EXPECT_TRUE(no_car_write(fake));
    EXPECT_EQ(fake.go_writes, 2u);
}

TEST(T1v3Run, TheDivisorIsNeverWrittenBack)
{
    Fake fake;
    run(fake);
    unsigned divisor_writes = 0;
    for (const auto &w : fake.writes)
        divisor_writes += w.first == T1V3_I2C1_CLK_DIVISOR;
    EXPECT_EQ(divisor_writes, 1u);
    EXPECT_EQ(fake.divisor, 0x00ff0001u);
}

TEST(T1v3Run, TheIdIsReadOnce)
{
    Fake fake;
    run(fake);
    unsigned id_pointer_writes = 0;
    for (const auto &w : fake.writes)
        id_pointer_writes += w.first == T1V3_I2C1_CMD_DATA1;
    EXPECT_EQ(id_pointer_writes, 1u);
}

TEST(T1v3Run, ElapsedTimeIsRecordedPerTransactionAsAReference)
{
    Fake fake;
    const auto r = run(fake);
    for (const auto &t : r.transaction) {
        EXPECT_GE(t.elapsed_us, 400u);
        EXPECT_LT(t.elapsed_us, 600u);
        EXPECT_EQ(t.busy_seen, 1);
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

TEST(T1v3Stops, GateMismatchWritesNothing)
{
    {
        Fake fake;
        fake.source = 0xc0000003;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_SOURCE_UNEXPECTED);
        EXPECT_TRUE(fake.writes.empty());
        EXPECT_EQ(fake.i2c_touches, 0u);
    }
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

TEST(T1v3Stops, AbnormalTimingRegistersStopBeforeTheFirstWrite)
{
    struct Case { uint32_t t0, t1; int stop; };
    const Case cases[] = {
        {0, 0x0a0a0a0a, SWITCH_T1V3_TIMING_ZERO},
        {0x204, 0, SWITCH_T1V3_TIMING_ZERO},
        {0xffffffff, 0x0a0a0a0a, SWITCH_T1V3_TIMING_UNREADABLE},
        {0x204, 0xffffffff, SWITCH_T1V3_TIMING_UNREADABLE},
        {0x205, 0x0a0a0a0a, SWITCH_T1V3_TIMING_UNEXPECTED},
        {0x104, 0x0a0a0a0a, SWITCH_T1V3_TIMING_UNEXPECTED},
        {0x00040204, 0x0a0a0a0a, SWITCH_T1V3_TIMING_UNEXPECTED},
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

TEST(T1v3Stops, AnyOtherTiming1ValueIsRecordedAndTheRunGoesOn)
{
    for (uint32_t t1 : {0x02020201u, 0x1f1f1f1fu, 0x00000001u}) {
        Fake fake;
        fake.timing_1 = t1;
        const auto r = run(fake);
        EXPECT_EQ(r.stop, 0);
        EXPECT_EQ(r.timing_1, t1);
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

TEST(T1v3Stops, ConfigLoadTimeoutStopsWithNoRestoreAndNoTransfer)
{
    Fake fake;
    fake.load_stuck = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_CONFIG_LOAD_TIMEOUT);
    EXPECT_EQ(fake.writes, divisor_and_load);
    EXPECT_EQ(fake.go_writes, 0u);
    EXPECT_EQ(r.transfers, 0u);
}

TEST(T1v3Stops, ConfigLoadWaitEndsByCountWhenTheCounterIsStopped)
{
    Fake fake;
    fake.load_stuck = true;
    fake.freeze_at_read = 14;  // the first CONFIG_LOAD poll is read 15 or later
    const auto r = run(fake);
    EXPECT_NE(r.stop, 0);
    EXPECT_LE(fake.writes.size(), 2u);
}

TEST(T1v3Stops, DivisorReadbackMismatchStopsWithNoRestore)
{
    Fake fake;
    fake.divisor_stuck = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_DIVISOR_READBACK);
    EXPECT_EQ(fake.writes, divisor_and_load);
    EXPECT_EQ(r.divisor_after, SWITCH_T1V3_DIVISOR_BEFORE);
    EXPECT_EQ(fake.go_writes, 0u);
}

TEST(T1v3Stops, BusyOrInterruptAfterTheLoadStopsBeforeAnyTransfer)
{
    {
        Fake fake;
        fake.busy_after_load = true;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_NOT_IDLE);
        EXPECT_EQ(fake.writes, divisor_and_load);
    }
    {
        Fake fake;
        fake.int_after_load = 2;
        EXPECT_EQ(run(fake).stop, SWITCH_T1V3_INT_CHANGED);
        EXPECT_EQ(fake.writes, divisor_and_load);
    }
}

TEST(T1v3Stops, NoAckStopsAfterTheFirstTransferWithNoRetry)
{
    Fake fake;
    fake.ack = false;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_NOACK);
    EXPECT_EQ(fake.go_writes, 1u);
    EXPECT_EQ(fake.writes.size(), 7u);
    EXPECT_EQ(r.transfers, 0u);
    EXPECT_FALSE(r.have_id);
    expect_only_listed_writes(fake);
}

TEST(T1v3Stops, BusyTimeoutStopsWithNoRetryAndNoReset)
{
    Fake fake;
    fake.duration = UINT64_C(1) << 40;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_BUSY_TIMEOUT);
    EXPECT_EQ(fake.go_writes, 1u);
    EXPECT_EQ(fake.writes.size(), 7u);
    expect_only_listed_writes(fake);
}

TEST(T1v3Stops, BusyPollEndsByCountWhenTheCounterStopsDuringIt)
{
    Fake fake;
    fake.duration = UINT64_C(1) << 40;
    fake.freeze_on_busy_poll = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_BUSY_TIMEOUT);
    EXPECT_EQ(fake.go_writes, 1u);
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
    fake.freeze_at_read = 12;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_COUNTER_STALLED);
    EXPECT_TRUE(fake.writes.empty());
}

TEST(T1v3Stops, CounterStoppingInTheSettleWaitStopsAfterTheGo)
{
    Fake fake;
    fake.freeze_at_read = 20;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_COUNTER_STALLED);
    EXPECT_EQ(fake.go_writes, 1u);
    expect_only_listed_writes(fake);
}

TEST(T1v3Stops, WrongIdIsReportedAfterTheTransfer)
{
    Fake fake;
    fake.id = 0x12;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1V3_ID_MISMATCH);
    EXPECT_EQ(fake.writes, full_sequence());
    EXPECT_EQ(r.id, 0x12);
}

TEST(T1v3Stops, EveryAnomalyRunWritesOnlyListedWordsAndNeverTheCar)
{
    for (int n = 0; n < 8; ++n) {
        Fake fake;
        fake.ack = n != 0;
        fake.load_stuck = n == 1;
        fake.divisor_stuck = n == 2;
        fake.busy_after_load = n == 3;
        fake.int_after_load = n == 4 ? 4 : 0;
        fake.id = n == 5 ? 0 : 0x55;
        fake.duration = n == 6 ? UINT64_C(1) << 40 : 400;
        fake.busy_on_status_read = n == 7 ? 3 : 0;
        run(fake);
        expect_only_listed_writes(fake);
        EXPECT_LE(fake.writes.size(), 11u);
    }
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
    EXPECT_NE(all.find("TIMING_0 00000204"), std::string::npos);
    EXPECT_NE(all.find("NOT RESTORED"), std::string::npos);
    EXPECT_NE(all.find("NOT AN SCL MEASUREMENT"), std::string::npos);
    EXPECT_NE(all.find("T1V3 COMPLETE CODE 0000"), std::string::npos);
    EXPECT_NE(all.find("WRITES 11"), std::string::npos);
    EXPECT_NE(all.find("ID(FE) 55"), std::string::npos);
    EXPECT_EQ(all.find("DEG"), std::string::npos);
}

TEST(T1v3Report, EveryStopHasItsOwnName)
{
    std::set<std::string> names;
    for (int stop = SWITCH_T1V3_I2C_CLOCK_OFF; stop <= SWITCH_T1V3_INT_CHANGED; ++stop) {
        const std::string name = switch_t1v3_stop_name(stop);
        EXPECT_NE(name, "UNKNOWN");
        EXPECT_TRUE(names.insert(name).second) << name;
    }
    EXPECT_EQ(std::string(switch_t1v3_stop_name(0)), "COMPLETE");
}

TEST(T1v3Report, AStoppedRunShowsTheStopLineInRed)
{
    Fake fake;
    fake.timing_0 = 0;
    const auto r = run(fake);
    switch_t1v3_line lines[SWITCH_T1V3_LINES];
    switch_t1v3_lines(&r, lines);
    EXPECT_NE(text(lines[6]).find("T1V3 STOPPED CODE 0606"), std::string::npos);
    EXPECT_EQ(lines[6].bad, 1);
}
