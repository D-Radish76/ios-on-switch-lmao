// T1 on the host: the allowlist, the run against a model of the I2C1 controller and the TMP451, and the screen text.
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "../t1.h"
#include "../t1_report.h"
}

static std::vector<std::pair<uint32_t, uint32_t>> gate_writes;
static std::vector<uint32_t> gate_reads;

extern "C" {
uint32_t switch_t1_read32(uint32_t address)
{
    gate_reads.push_back(address);
    return 0xabcd;
}
void switch_t1_write32(uint32_t address, uint32_t value)
{
    gate_writes.emplace_back(address, value);
}
}

namespace {
bool is_i2c(unsigned reg)
{
    return reg >= T1_I2C1_CNFG;
}

struct Fake {
    bool clock_on = true, in_reset = false, ack = true, stuck_busy = false, load_stuck = false;
    uint32_t divisor = SWITCH_T1_DIVISOR_HEKATE, status_busy = 0;
    uint8_t id = 0x55, config = 0xc0, rate = 0x08, local = 0x12, remote = 0x34;
    uint64_t now = 0;
    uint32_t addr0 = 0, data1 = 0, cnfg = 0, noack_bits = 0;
    uint8_t pointer = 0;
    int busy_polls = 0;
    unsigned i2c_touches = 0, go_writes = 0;
    std::vector<std::pair<unsigned, uint32_t>> writes;

    uint8_t value() const
    {
        switch (pointer) {
        case 0xfe: return id;
        case 0x09: return config;
        case 0x0a: return rate;
        case 0x00: return local;
        case 0x01: return remote;
        default: return 0xff;
        }
    }
    uint32_t read(unsigned reg)
    {
        now += 50;
        if (is_i2c(reg))
            ++i2c_touches;
        switch (reg) {
        case T1_CAR_RST_DEVICES_L: return in_reset ? (1u << 12) : 0;
        case T1_CAR_CLK_OUT_ENB_L: return clock_on ? (1u << 12) : 0;
        case T1_CAR_CLK_SOURCE_I2C1: return 0x6;
        case T1_I2C1_CLK_DIVISOR: return divisor;
        case T1_I2C1_STATUS:
            if (stuck_busy && busy_polls)
                return 0x100;
            if (busy_polls > 0) {
                --busy_polls;
                return 0x100;
            }
            return noack_bits | status_busy;
        case T1_I2C1_CONFIG_LOAD: return load_stuck ? 1 : 0;
        case T1_I2C1_CMD_DATA1: return data1;
        default: return 0;
        }
    }
    void write(unsigned reg, uint32_t v)
    {
        now += 50;
        writes.emplace_back(reg, v);
        if (is_i2c(reg))
            ++i2c_touches;
        if (reg == T1_I2C1_CMD_ADDR0)
            addr0 = v;
        else if (reg == T1_I2C1_CMD_DATA1)
            data1 = v;
        else if (reg == T1_I2C1_CNFG && (v & 0x200)) {
            ++go_writes;
            busy_polls = stuck_busy ? 1 : 3;
            noack_bits = ack ? 0 : 1;
            if (ack && (addr0 & 1))
                data1 = value();
            else if (ack)
                pointer = static_cast<uint8_t>(data1);
        }
    }
};

uint32_t f_read(void *c, unsigned r) { return static_cast<Fake *>(c)->read(r); }
void f_write(void *c, unsigned r, uint32_t v) { static_cast<Fake *>(c)->write(r, v); }
uint64_t f_now(void *c) { return static_cast<Fake *>(c)->now; }

switch_t1_result run(Fake &fake)
{
    const switch_t1_io io = {&fake, f_read, f_write, f_now};
    switch_t1_result result;
    switch_t1_run(&io, &result);
    return result;
}

std::string text(const switch_t1_line &line) { return line.text.text; }
}  // namespace

TEST(T1Allowlist, OnlyListedRegistersAreReadable)
{
    for (unsigned reg = 0; reg < T1_REG_COUNT + 4; ++reg) {
        const bool writeOnly = reg == T1_I2C1_CMD_ADDR0;
        EXPECT_EQ(switch_t1_may_read(reg), writeOnly || reg >= T1_REG_COUNT ? 0 : 1);
    }
}

TEST(T1Allowlist, WritesAreTheI2cTransferRegistersWithTheirValues)
{
    std::set<std::pair<unsigned, uint32_t>> allowed = {
        {T1_I2C1_CMD_ADDR0, 0x98}, {T1_I2C1_CMD_ADDR0, 0x99}, {T1_I2C1_CNFG, 0x2800}, {T1_I2C1_CNFG, 0x2840},
        {T1_I2C1_CNFG, 0x2a00},    {T1_I2C1_CNFG, 0x2a40},    {T1_I2C1_CONFIG_LOAD, 0x25},
    };
    for (uint8_t reg : switch_t1_tmp451_register)
        allowed.insert({T1_I2C1_CMD_DATA1, reg});
    for (unsigned reg = 0; reg < T1_REG_COUNT + 2; ++reg)
        for (uint32_t value = 0; value < 0x3000; ++value)
            EXPECT_EQ(switch_t1_may_write(reg, value), allowed.count({reg, value}) ? 1 : 0);
    EXPECT_EQ(switch_t1_may_write(T1_I2C1_CNFG, 0xffffffffu), 0);
}

TEST(T1Allowlist, NothingIsWritableOutsideI2c1)
{
    for (unsigned reg = 0; reg < T1_I2C1_CNFG; ++reg)
        for (uint32_t value : {0u, 1u, 0x25u, 0x2800u, 0x98u, 0xfeu})
            EXPECT_EQ(switch_t1_may_write(reg, value), 0);
    EXPECT_EQ(switch_t1_may_write(T1_I2C1_CLK_DIVISOR, 0x50001), 0);
    EXPECT_EQ(switch_t1_may_write(T1_I2C1_STATUS, 0), 0);
    EXPECT_EQ(switch_t1_may_write(T1_I2C1_INT_STATUS, 0xffffffffu), 0);
}

TEST(T1Allowlist, TmpRegisterListHasNoWriteOnlyOrStatusRegister)
{
    for (uint8_t reg : switch_t1_tmp451_register) {
        EXPECT_NE(reg, 0x02);  // status: a read clears latched flags
        EXPECT_NE(reg, 0x0f);  // one shot: T2
        EXPECT_NE(reg, 0x03);
    }
}

TEST(T1Allowlist, AddressesAreTheReviewedOnes)
{
    EXPECT_EQ(switch_t1_address(T1_CAR_CLK_OUT_ENB_L), 0x60006010u);
    EXPECT_EQ(switch_t1_address(T1_CAR_RST_DEVICES_L), 0x60006004u);
    EXPECT_EQ(switch_t1_address(T1_CAR_CLK_SOURCE_I2C1), 0x60006124u);
    EXPECT_EQ(switch_t1_address(T1_CAR_CLK_SOURCE_PWM), 0x60006110u);
    EXPECT_EQ(switch_t1_address(T1_I2C1_CNFG), 0x7000c000u);
    EXPECT_EQ(switch_t1_address(T1_I2C1_CMD_ADDR0), 0x7000c004u);
    EXPECT_EQ(switch_t1_address(T1_I2C1_CMD_DATA1), 0x7000c00cu);
    EXPECT_EQ(switch_t1_address(T1_I2C1_STATUS), 0x7000c01cu);
    EXPECT_EQ(switch_t1_address(T1_I2C1_INT_STATUS), 0x7000c068u);
    EXPECT_EQ(switch_t1_address(T1_I2C1_CLK_DIVISOR), 0x7000c06cu);
    EXPECT_EQ(switch_t1_address(T1_I2C1_CONFIG_LOAD), 0x7000c08cu);
    EXPECT_EQ(switch_t1_address(T1_PINMUX_I2C1_SCL), 0x700030bcu);
    EXPECT_EQ(switch_t1_address(T1_PINMUX_I2C1_SDA), 0x700030c0u);
    EXPECT_EQ(switch_t1_address(T1_REG_COUNT), 0u);
}

TEST(T1Gate, RefusedAccessesNeverReachTheDevice)
{
    gate_writes.clear();
    gate_reads.clear();
    switch_t1_access(T1_I2C1_CLK_DIVISOR, 1, 0x50001);
    switch_t1_access(T1_CAR_CLK_OUT_ENB_L, 1, 0);
    switch_t1_access(T1_I2C1_CNFG, 1, 0x2801);
    switch_t1_access(T1_I2C1_CMD_ADDR0, 0, 0);
    switch_t1_access(T1_REG_COUNT, 0, 0);
    switch_t1_access(1000, 1, 0x98);
    EXPECT_EQ(gate_writes.size(), 0u);
    EXPECT_EQ(gate_reads.size(), 0u);
}

TEST(T1Gate, AllowedAccessesUseTheTableAddress)
{
    gate_writes.clear();
    gate_reads.clear();
    EXPECT_EQ(switch_t1_access(T1_I2C1_STATUS, 0, 0), 0xabcdu);
    switch_t1_access(T1_I2C1_CONFIG_LOAD, 1, 0x25);
    EXPECT_EQ(gate_reads.size(), 1u);
    EXPECT_EQ(gate_reads[0], 0x7000c01cu);
    EXPECT_EQ(gate_writes.size(), 1u);
    EXPECT_EQ(gate_writes[0].first, 0x7000c08cu);
    EXPECT_EQ(gate_writes[0].second, 0x25u);
}

TEST(T1Run, HealthyBus)
{
    Fake fake;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, 0);
    EXPECT_EQ(r.transfers, 10u);
    EXPECT_EQ(r.have, 0x1fu);
    EXPECT_EQ(r.value[0], 0x55);
    EXPECT_EQ(r.value[1], 0xc0);
    EXPECT_EQ(r.value[2], 0x08);
    EXPECT_EQ(r.value[3], 0x12);
    EXPECT_EQ(r.value[4], 0x34);
    EXPECT_EQ(fake.writes.size(), 45u);
    EXPECT_EQ(fake.go_writes, 10u);
}

TEST(T1Run, EveryWriteIsAPointerWriteOrATransferSetup)
{
    Fake fake;
    run(fake);
    for (auto [reg, value] : fake.writes) {
        EXPECT_EQ(switch_t1_may_write(reg, value), 1);
        if (reg == T1_I2C1_CNFG)
            EXPECT_EQ(value & 0xe, 0u);  // one byte per transfer, so a write is the register pointer alone
    }
    std::set<uint32_t> pointers;
    for (auto [reg, value] : fake.writes)
        if (reg == T1_I2C1_CMD_DATA1)
            pointers.insert(value);
    EXPECT_EQ(pointers, (std::set<uint32_t>{0x00, 0x01, 0x09, 0x0a, 0xfe}));
    EXPECT_EQ(fake.config, 0xc0);  // the model's sensor configuration is as it was
}

TEST(T1Run, ClockOffTouchesNoI2cRegister)
{
    Fake fake;
    fake.clock_on = false;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_I2C_CLOCK_OFF);
    EXPECT_EQ(fake.i2c_touches, 0u);
    EXPECT_EQ(fake.writes.size(), 0u);
}

TEST(T1Run, ResetTouchesNoI2cRegister)
{
    Fake fake;
    fake.in_reset = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_I2C_IN_RESET);
    EXPECT_EQ(fake.i2c_touches, 0u);
}

TEST(T1Run, BusyBusWritesNothingAndDoesNotClearIt)
{
    Fake fake;
    fake.status_busy = 0x100;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_BUS_BUSY);
    EXPECT_EQ(fake.writes.size(), 0u);
}

TEST(T1Run, DivisorHekateDidNotSetIsNotWritten)
{
    Fake fake;
    fake.divisor = 0x00050002;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_DIVISOR_UNEXPECTED);
    EXPECT_EQ(fake.writes.size(), 0u);
    EXPECT_EQ(r.divisor, 0x00050002u);
}

TEST(T1Run, NoAckStopsAtTheFirstTransferWithoutRetry)
{
    Fake fake;
    fake.ack = false;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_NOACK);
    EXPECT_EQ(r.transfers, 0u);
    EXPECT_EQ(fake.go_writes, 1u);
    EXPECT_EQ(r.have, 0u);
}

TEST(T1Run, BusyThatNeverClearsStopsWithinTheBoundAndDoesNotRetry)
{
    Fake fake;
    fake.stuck_busy = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_BUSY_TIMEOUT);
    EXPECT_EQ(fake.go_writes, 1u);
    EXPECT_LE(fake.now, 100000u + 1000u);
}

TEST(T1Run, ConfigLoadThatNeverFinishesStopsBeforeGo)
{
    Fake fake;
    fake.load_stuck = true;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_CONFIG_LOAD_TIMEOUT);
    EXPECT_EQ(fake.go_writes, 0u);
}

TEST(T1Run, WrongManufacturerStopsAfterTheFirstRegister)
{
    Fake fake;
    fake.id = 0x00;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, SWITCH_T1_ID_MISMATCH);
    EXPECT_EQ(r.transfers, 2u);
    EXPECT_EQ(r.have, 1u);
}

TEST(T1Report, HealthyRunNeverPrintsATemperature)
{
    Fake fake;
    const auto r = run(fake);
    switch_t1_line lines[SWITCH_T1_LINES];
    switch_t1_lines(&r, lines);
    std::string all;
    for (const auto &line : lines) {
        all += text(line) + "\n";
        EXPECT_LE(line.text.length, SWITCH_DIAG_LINE_MAX);
        EXPECT_EQ(line.bad, 0);
    }
    EXPECT_NE(all.find("NOT A TEMPERATURE"), std::string::npos);
    EXPECT_NE(all.find("ID(FE) 55"), std::string::npos);
    EXPECT_NE(all.find("T1 COMPLETE CODE 0000"), std::string::npos);
    EXPECT_EQ(all.find(" C\n"), std::string::npos);
    EXPECT_EQ(all.find("DEG"), std::string::npos);
    EXPECT_NE(all.find("PWM CLOCK OFF"), std::string::npos);
}

TEST(T1Report, ClockOffShowsOnlyTheCarLines)
{
    Fake fake;
    fake.clock_on = false;
    const auto r = run(fake);
    switch_t1_line lines[SWITCH_T1_LINES];
    switch_t1_lines(&r, lines);
    EXPECT_EQ(lines[1].bad, 1);
    EXPECT_EQ(lines[2].text.length, 0u);
    EXPECT_EQ(lines[4].text.length, 0u);
    EXPECT_EQ(lines[6].bad, 1);
    EXPECT_NE(text(lines[6]).find("0501"), std::string::npos);
}

TEST(T1Report, EveryStopHasAName)
{
    for (int stop : {0x501, 0x502, 0x503, 0x504, 0x505, 0x506, 0x507, 0x508, 0x509, 0x50a, 0x50b})
        EXPECT_NE(std::string(switch_t1_stop_name(stop)), "UNKNOWN");
}
