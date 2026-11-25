// T1-R on the host: the read allowlist, the run against a model of CAR and I2C1, and the screen text.
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../t1r.h"
#include "../t1r_report.h"
}

static std::vector<uint32_t> raw_reads;

extern "C" uint32_t switch_t1r_read32(uint32_t address)
{
    raw_reads.push_back(address);
    return 0xabcd;
}

namespace {
bool is_i2c1(unsigned reg)
{
    return reg >= T1R_I2C1_CNFG;
}

struct Fake {
    uint32_t rst = 0, enb = 1u << 12;
    uint32_t source[6] = {0xc0000003, 0xc0000000, 0xc0000000, 0xc0000003, 0xc0000000, 0xc0000003};
    uint32_t i2c1_value = 0x11110000;
    std::vector<unsigned> reads;
    unsigned i2c1_touches = 0;

    uint32_t read(unsigned reg)
    {
        reads.push_back(reg);
        if (is_i2c1(reg))
            ++i2c1_touches;
        if (reg == T1R_CAR_RST_DEVICES_L)
            return rst;
        if (reg == T1R_CAR_CLK_OUT_ENB_L)
            return enb;
        if (reg >= T1R_CAR_SOURCE_I2C1 && reg <= T1R_CAR_SOURCE_I2C6)
            return source[reg - T1R_CAR_SOURCE_I2C1];
        if (reg == T1R_CAR_PLLP_BASE)
            return 0xaaaa0001;
        if (reg == T1R_CAR_PLLP_OUTA)
            return 0xaaaa0002;
        if (reg == T1R_CAR_PLLP_OUTB)
            return 0xaaaa0003;
        return i2c1_value + reg;
    }
};

uint32_t fake_read(void *context, unsigned reg)
{
    return static_cast<Fake *>(context)->read(reg);
}

switch_t1r_result run(Fake &fake)
{
    const switch_t1r_io io = {&fake, fake_read};
    switch_t1r_result result;
    switch_t1r_run(&io, &result);
    return result;
}

std::string text(const switch_t1r_line &line)
{
    return std::string(line.text.text, line.text.length);
}

std::string screen(const switch_t1r_result &result)
{
    switch_t1r_line lines[SWITCH_T1R_LINES];
    switch_t1r_lines(&result, lines);
    std::string all;
    for (const auto &line : lines)
        all += text(line) + "\n";
    return all;
}
} // namespace

TEST(T1RAllowlist, EveryRegisterHasItsListedAddressAndNothingElseIsReadable)
{
    const uint32_t expected[T1R_REG_COUNT] = {
        0x60006004, 0x60006010, 0x60006124, 0x60006198, 0x600061b8, 0x600063c4, 0x60006128, 0x6000665c,
        0x600060a0, 0x600060a4, 0x600060a8, 0x7000c000, 0x7000c01c, 0x7000c068, 0x7000c06c, 0x7000c08c};
    for (unsigned reg = 0; reg < T1R_REG_COUNT; ++reg) {
        EXPECT_EQ(switch_t1r_address(reg), expected[reg]) << reg;
        EXPECT_TRUE(switch_t1r_may_read(reg)) << reg;
    }
    for (unsigned reg = T1R_REG_COUNT; reg < T1R_REG_COUNT + 64; ++reg) {
        EXPECT_EQ(switch_t1r_address(reg), 0u) << reg;
        EXPECT_FALSE(switch_t1r_may_read(reg)) << reg;
    }
    EXPECT_EQ(switch_t1r_may_read(~0u), 0);
}

TEST(T1RAllowlist, NoListedAddressIsAReadToClearOrDataRegister)
{
    const std::set<uint32_t> banned = {0x7000c00c, 0x7000c010, 0x7000c050, 0x7000c054, 0x7000c088, 0x7000c094, 0x7000c098};
    for (unsigned reg = 0; reg < T1R_REG_COUNT; ++reg)
        EXPECT_EQ(banned.count(switch_t1r_address(reg)), 0u) << reg;
}

TEST(T1RGate, ARefusedReadIsNotMadeAndAnAllowedOneReachesTheRawRead)
{
    raw_reads.clear();
    EXPECT_EQ(switch_t1r_access(T1R_REG_COUNT), 0u);
    EXPECT_EQ(switch_t1r_access(~0u), 0u);
    EXPECT_TRUE(raw_reads.empty());
    EXPECT_EQ(switch_t1r_access(T1R_CAR_SOURCE_I2C1), 0xabcdu);
    ASSERT_EQ(raw_reads.size(), 1u);
    EXPECT_EQ(raw_reads[0], 0x60006124u);
}

TEST(T1RGate, ARunOnTheGateReadsEachListedAddressOnceAndNothingElse)
{
    raw_reads.clear();
    switch_t1r_result result;
    const switch_t1r_io io = {nullptr, [](void *, unsigned reg) { return switch_t1r_access(reg); }};
    switch_t1r_run(&io, &result);
    std::multiset<uint32_t> seen(raw_reads.begin(), raw_reads.end());
    for (unsigned reg = 0; reg < T1R_CAR_PLLP_OUTB + 1; ++reg)
        EXPECT_EQ(seen.count(switch_t1r_address(reg)), 1u) << reg;
    for (uint32_t address : seen)
        EXPECT_TRUE((address >= 0x60006000 && address < 0x60007000) || (address >= 0x7000c000 && address < 0x7000c100)) << address;
}

TEST(T1RRun, AHealthyStateReadsCarFirstThenFiveI2c1Registers)
{
    Fake fake;
    const auto r = run(fake);
    EXPECT_EQ(r.stop, 0);
    EXPECT_EQ(r.i2c1, SWITCH_T1R_I2C1_READ);
    ASSERT_EQ(fake.reads.size(), 16u);
    for (unsigned i = 0; i < 11; ++i)
        EXPECT_FALSE(is_i2c1(fake.reads[i])) << i;
    EXPECT_EQ(fake.i2c1_touches, 5u);
    EXPECT_EQ(r.source[0], 0xc0000003u);
    EXPECT_EQ(r.pllp_base, 0xaaaa0001u);
}

TEST(T1RRun, AClockThatIsOffTouchesNoI2c1Register)
{
    Fake fake;
    fake.enb = 0;
    const auto r = run(fake);
    EXPECT_EQ(r.i2c1, SWITCH_T1R_I2C1_CLOCK_OFF);
    EXPECT_EQ(fake.i2c1_touches, 0u);
    EXPECT_EQ(fake.reads.size(), 11u);
    EXPECT_EQ(r.cnfg, 0u);
}

TEST(T1RRun, ABlockInResetTouchesNoI2c1Register)
{
    Fake fake;
    fake.rst = 1u << 12;
    const auto r = run(fake);
    EXPECT_EQ(r.i2c1, SWITCH_T1R_I2C1_IN_RESET);
    EXPECT_EQ(fake.i2c1_touches, 0u);
}

TEST(T1RRun, AClockOffWithTheResetBitSetStillTouchesNothing)
{
    Fake fake;
    fake.enb = 0;
    fake.rst = 1u << 12;
    run(fake);
    EXPECT_EQ(fake.i2c1_touches, 0u);
}

TEST(T1RRun, TheSixSourceWordsAreReadFromTheirOwnRegisters)
{
    Fake fake;
    for (unsigned i = 0; i < 6; ++i)
        fake.source[i] = 0x1000 + i;
    const auto r = run(fake);
    for (unsigned i = 0; i < 6; ++i)
        EXPECT_EQ(r.source[i], 0x1000u + i);
}

TEST(T1RReport, ARefusedReadShowsAStopCodeAndNotACompletion)
{
    Fake fake;
    auto r = run(fake);
    r.stop = SWITCH_T1R_DENIED;
    const auto all = screen(r);
    EXPECT_NE(all.find("T1R STOPPED CODE 0601"), std::string::npos) << all;
    EXPECT_NE(all.find("ALLOWLIST REFUSED A READ"), std::string::npos) << all;
    EXPECT_EQ(all.find("T1R COMPLETE"), std::string::npos) << all;
}

TEST(T1RHekate, TheExpectedSourceWordsAreHekatesTable)
{
    const uint32_t expected[SWITCH_T1R_BUSES] = {0xc0000003, 0xc0000000, 0xc0000000, 0xc0000003, 0xc0000000, 0xc0000003};
    for (unsigned bus = 0; bus < SWITCH_T1R_BUSES; ++bus)
        EXPECT_EQ(switch_t1r_hekate_source(bus), expected[bus]) << bus;
    EXPECT_EQ(switch_t1r_hekate_source(SWITCH_T1R_BUSES), 0u);
}

TEST(T1RReport, TheRecordedErista0507StateShowsI2c1AsDifferent)
{
    Fake fake;
    fake.rst = 0x049102CA;
    fake.enb = 0x9802D131;
    fake.source[0] = 0x00000003;
    const auto r = run(fake);
    const auto all = screen(r);
    EXPECT_NE(all.find("CLK_SOURCE_I2C1 00000003"), std::string::npos) << all;
    EXPECT_NE(all.find("HEKATE WRITES C0000003. DIFFERENT"), std::string::npos) << all;
    EXPECT_NE(all.find("CLK_SOURCE_I2C5 C0000000"), std::string::npos) << all;
    EXPECT_NE(all.find("I2C1 CLOCK ON OUT OF RESET"), std::string::npos) << all;
}

TEST(T1RReport, PllpIsRawWordsAndNoFrequencyOrTemperatureIsClaimed)
{
    Fake fake;
    const auto all = screen(run(fake));
    EXPECT_NE(all.find("PLLP BASE AAAA0001 OUTA AAAA0002 OUTB AAAA0003"), std::string::npos) << all;
    EXPECT_NE(all.find("NO FREQUENCY CLAIMED"), std::string::npos);
    EXPECT_EQ(all.find("MHZ"), std::string::npos);
    EXPECT_EQ(all.find("KHZ"), std::string::npos);
    EXPECT_NE(all.find("NO I2C TRANSFER. NO REGISTER WRITE"), std::string::npos);
}

TEST(T1RReport, ASkippedI2c1StatesWhy)
{
    Fake fake;
    fake.enb = 0;
    EXPECT_NE(screen(run(fake)).find("I2C1 REGISTERS NOT READ: CLOCK OFF"), std::string::npos);
    Fake in_reset;
    in_reset.rst = 1u << 12;
    EXPECT_NE(screen(run(in_reset)).find("I2C1 REGISTERS NOT READ: IN RESET"), std::string::npos);
}

TEST(T1RReport, EveryLineFitsTheScreen)
{
    Fake fake;
    switch_t1r_line lines[SWITCH_T1R_LINES];
    const auto r = run(fake);
    switch_t1r_lines(&r, lines);
    for (const auto &line : lines)
        EXPECT_LE(line.text.length, 106u) << text(line);
}
