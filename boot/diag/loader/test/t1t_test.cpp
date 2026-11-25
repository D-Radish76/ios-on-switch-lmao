// T1-T on the host: the read allowlist, the run against a model of CAR, I2C1 and I2C5, and the screen text.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../t1t.h"
#include "../t1t_report.h"
}

static std::vector<uint32_t> raw_reads;

extern "C" uint32_t switch_t1t_read32(uint32_t address)
{
    raw_reads.push_back(address);
    return 0xabcd;
}

namespace {
constexpr unsigned CAR_WORDS = 11;
constexpr uint32_t CLK_I2C1 = 1u << 12, CLK_I2C5 = 1u << 15;

struct Fake {
    uint32_t rst_l = 0, rst_h = 0, enb_l = CLK_I2C1, enb_h = CLK_I2C5;
    std::map<unsigned, uint32_t> value;
    std::vector<unsigned> reads;

    uint32_t read(unsigned reg)
    {
        reads.push_back(reg);
        switch (reg) {
        case T1T_CAR_RST_DEVICES_L: return rst_l;
        case T1T_CAR_RST_DEVICES_H: return rst_h;
        case T1T_CAR_CLK_OUT_ENB_L: return enb_l;
        case T1T_CAR_CLK_OUT_ENB_H: return enb_h;
        }
        auto it = value.find(reg);
        return it == value.end() ? 0x70000000u + reg : it->second;
    }
    unsigned i2c_reads(unsigned block) const
    {
        unsigned n = 0;
        for (unsigned reg : reads)
            n += reg >= SWITCH_T1T_I2C_REG(block, 0) && reg < SWITCH_T1T_I2C_REG(block, 0) + T1T_BLOCK_WORDS;
        return n;
    }
};

uint32_t fake_read(void *context, unsigned reg)
{
    return static_cast<Fake *>(context)->read(reg);
}

switch_t1t_result run(Fake &fake)
{
    switch_t1t_io io = {&fake, fake_read};
    switch_t1t_result result;
    switch_t1t_run(&io, &result);
    return result;
}

std::string text(const switch_t1t_line &line)
{
    return std::string(line.text.text, line.text.length);
}

std::vector<std::string> screen(const switch_t1t_result &result)
{
    switch_t1t_line lines[SWITCH_T1T_LINES];
    switch_t1t_lines(&result, lines);
    std::vector<std::string> out;
    for (const auto &line : lines)
        out.push_back(text(line));
    return out;
}

bool has(const std::vector<std::string> &lines, const std::string &needle)
{
    for (const auto &line : lines)
        if (line.find(needle) != std::string::npos)
            return true;
    return false;
}
} // namespace

TEST(T1tAllowlist, EveryRegisterHasOneAddressInTheThreePages)
{
    std::set<uint32_t> seen;
    for (unsigned reg = 0; reg < T1T_REG_COUNT; ++reg) {
        EXPECT_TRUE(switch_t1t_may_read(reg));
        const uint32_t address = switch_t1t_address(reg);
        EXPECT_TRUE((address >> 12) == 0x60006 || (address >> 12) == 0x7000c || (address >> 12) == 0x7000d);
        EXPECT_EQ(address & 3u, 0u);
        EXPECT_TRUE(seen.insert(address).second);
    }
    EXPECT_EQ(seen.size(), (size_t)T1T_REG_COUNT);
}

TEST(T1tAllowlist, NothingPastTheTableIsReadable)
{
    for (unsigned reg : {(unsigned)T1T_REG_COUNT, T1T_REG_COUNT + 1u, 0x1000u, 0xffffffffu}) {
        EXPECT_FALSE(switch_t1t_may_read(reg));
        EXPECT_EQ(switch_t1t_address(reg), 0u);
    }
}

TEST(T1tAllowlist, TheTimingRegistersAreAtTheirOffsets)
{
    EXPECT_EQ(switch_t1t_address(SWITCH_T1T_I2C_REG(0, T1T_TIMING_0)), 0x7000c094u);
    EXPECT_EQ(switch_t1t_address(SWITCH_T1T_I2C_REG(0, T1T_TIMING_1)), 0x7000c098u);
    EXPECT_EQ(switch_t1t_address(SWITCH_T1T_I2C_REG(1, T1T_TIMING_0)), 0x7000d094u);
    EXPECT_EQ(switch_t1t_address(SWITCH_T1T_I2C_REG(1, T1T_TIMING_1)), 0x7000d098u);
    EXPECT_EQ(switch_t1t_address(SWITCH_T1T_I2C_REG(0, T1T_CONFIG_LOAD)), 0x7000c08cu);
    EXPECT_EQ(switch_t1t_address(SWITCH_T1T_I2C_REG(1, T1T_CLK_DIVISOR)), 0x7000d06cu);
    EXPECT_EQ(switch_t1t_address(T1T_CAR_SOURCE_I2C1), 0x60006124u);
    EXPECT_EQ(switch_t1t_address(T1T_CAR_SOURCE_I2C5), 0x60006128u);
    EXPECT_EQ(switch_t1t_address(T1T_CAR_PLLP_BASE), 0x600060a0u);
}

TEST(T1tAllowlist, NoRegisterIsACommandOrDataRegister)
{
    std::set<uint32_t> forbidden;
    for (uint32_t base : {0x7000c000u, 0x7000d000u})
        for (uint32_t offset : {0x04u, 0x08u, 0x0cu, 0x10u, 0x88u})
            forbidden.insert(base + offset);
    for (unsigned reg = 0; reg < T1T_REG_COUNT; ++reg)
        EXPECT_EQ(forbidden.count(switch_t1t_address(reg)), 0u);
}

TEST(T1tGate, ARefusedReadNeverReachesTheDevice)
{
    raw_reads.clear();
    EXPECT_EQ(switch_t1t_access(T1T_REG_COUNT), 0u);
    EXPECT_EQ(switch_t1t_access(0xffffffffu), 0u);
    EXPECT_TRUE(raw_reads.empty());
}

TEST(T1tGate, AnAllowedReadReachesTheDeviceAtItsAddressOnly)
{
    for (unsigned reg = 0; reg < T1T_REG_COUNT; ++reg) {
        raw_reads.clear();
        EXPECT_EQ(switch_t1t_access(reg), 0xabcdu);
        ASSERT_EQ(raw_reads.size(), 1u);
        EXPECT_EQ(raw_reads[0], switch_t1t_address(reg));
    }
}

TEST(T1tRun, BothControllersAreReadWhenOnAndOutOfReset)
{
    Fake fake;
    const auto result = run(fake);
    EXPECT_EQ(result.stop, 0);
    EXPECT_EQ(result.reads, CAR_WORDS + 2 * T1T_BLOCK_WORDS);
    EXPECT_EQ(fake.reads.size(), (size_t)result.reads);
    EXPECT_EQ(result.block[0].state, SWITCH_T1T_BLOCK_READ);
    EXPECT_EQ(result.block[1].state, SWITCH_T1T_BLOCK_READ);
}

TEST(T1tRun, TheCarIsReadBeforeAnyControllerRegister)
{
    Fake fake;
    run(fake);
    for (unsigned i = 0; i < CAR_WORDS; ++i)
        EXPECT_LT(fake.reads[i], (unsigned)T1T_I2C_FIRST);
    for (unsigned i = CAR_WORDS; i < fake.reads.size(); ++i)
        EXPECT_GE(fake.reads[i], (unsigned)T1T_I2C_FIRST);
}

TEST(T1tRun, ThisControllersClockOffMeansNoRegisterOfItIsRead)
{
    Fake fake;
    fake.enb_l = 0;
    const auto result = run(fake);
    EXPECT_EQ(fake.i2c_reads(0), 0u);
    EXPECT_EQ(fake.i2c_reads(1), (unsigned)T1T_BLOCK_WORDS);
    EXPECT_EQ(result.block[0].state, SWITCH_T1T_BLOCK_CLOCK_OFF);
    EXPECT_EQ(result.block[1].state, SWITCH_T1T_BLOCK_READ);
}

TEST(T1tRun, ThisControllersResetMeansNoRegisterOfItIsRead)
{
    Fake fake;
    fake.rst_h = CLK_I2C5;
    const auto result = run(fake);
    EXPECT_EQ(fake.i2c_reads(0), (unsigned)T1T_BLOCK_WORDS);
    EXPECT_EQ(fake.i2c_reads(1), 0u);
    EXPECT_EQ(result.block[0].state, SWITCH_T1T_BLOCK_READ);
    EXPECT_EQ(result.block[1].state, SWITCH_T1T_BLOCK_IN_RESET);
}

TEST(T1tRun, ClockOffWinsOverReset)
{
    Fake fake;
    fake.enb_l = 0;
    fake.rst_l = CLK_I2C1;
    EXPECT_EQ(run(fake).block[0].state, SWITCH_T1T_BLOCK_CLOCK_OFF);
}

TEST(T1tRun, TheOtherControllersBitDoesNotGateThisOne)
{
    Fake fake;
    fake.enb_l = 0;
    fake.enb_h = 0;
    fake.rst_l = CLK_I2C5;
    fake.rst_h = CLK_I2C1;
    run(fake);
    EXPECT_EQ(fake.i2c_reads(0), 0u);
    EXPECT_EQ(fake.i2c_reads(1), 0u);
    fake = Fake();
    fake.rst_l = CLK_I2C5;
    fake.rst_h = CLK_I2C1;
    run(fake);
    EXPECT_EQ(fake.i2c_reads(0), (unsigned)T1T_BLOCK_WORDS);
    EXPECT_EQ(fake.i2c_reads(1), (unsigned)T1T_BLOCK_WORDS);
}

TEST(T1tRun, BothOffReadsOnlyTheCar)
{
    Fake fake;
    fake.enb_l = fake.enb_h = 0;
    const auto result = run(fake);
    EXPECT_EQ(result.reads, CAR_WORDS);
    EXPECT_EQ(result.stop, 0);
}

TEST(T1tRun, ReadsEachRegisterAtMostOnce)
{
    Fake fake;
    run(fake);
    std::set<unsigned> seen(fake.reads.begin(), fake.reads.end());
    EXPECT_EQ(seen.size(), fake.reads.size());
}

TEST(T1tRun, TheWordsAreKeptRaw)
{
    Fake fake;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_0)] = 0xdeadbe01;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_1)] = 0xcafef00d;
    fake.value[SWITCH_T1T_I2C_REG(1, T1T_TIMING_0)] = 0x00000204;
    fake.value[SWITCH_T1T_I2C_REG(1, T1T_TIMING_1)] = 0;
    fake.value[T1T_CAR_PLLP_BASE] = 0x48115408;
    const auto result = run(fake);
    EXPECT_EQ(result.block[0].word[T1T_TIMING_0], 0xdeadbe01u);
    EXPECT_EQ(result.block[0].word[T1T_TIMING_1], 0xcafef00du);
    EXPECT_EQ(result.block[1].word[T1T_TIMING_0], 0x204u);
    EXPECT_EQ(result.block[1].word[T1T_TIMING_1], 0u);
    EXPECT_EQ(result.pllp_base, 0x48115408u);
}

TEST(T1tRun, AllSixSourcesAreKept)
{
    Fake fake;
    for (unsigned bus = 0; bus < SWITCH_T1T_BUSES; ++bus)
        fake.value[T1T_CAR_SOURCE_I2C1 + bus] = 0x100 + bus;
    const auto result = run(fake);
    for (unsigned bus = 0; bus < SWITCH_T1T_BUSES; ++bus)
        EXPECT_EQ(result.source[bus], 0x100u + bus);
}

TEST(T1tRun, ASkippedBlockHoldsNoStaleWords)
{
    Fake fake;
    fake.enb_h = 0;
    switch_t1t_result result;
    for (volatile uint8_t *p = (uint8_t *)&result; p < (uint8_t *)(&result + 1); ++p)
        *p = 0xee;
    switch_t1t_io io = {&fake, fake_read};
    switch_t1t_run(&io, &result);
    for (unsigned word = 0; word < T1T_BLOCK_WORDS; ++word)
        EXPECT_EQ(result.block[1].word[word], 0u);
}

TEST(T1tRun, TheRunTouchesNothingButTheListedRegisters)
{
    Fake fake;
    run(fake);
    for (unsigned reg : fake.reads)
        EXPECT_TRUE(switch_t1t_may_read(reg));
}

TEST(T1tRun, TheRunThroughTheGateReadsOnlyAllowlistedAddresses)
{
    std::set<uint32_t> allowed;
    for (unsigned reg = 0; reg < T1T_REG_COUNT; ++reg)
        allowed.insert(switch_t1t_address(reg));
    raw_reads.clear();
    switch_t1t_io io = {nullptr, [](void *, unsigned reg) { return switch_t1t_access(reg); }};
    switch_t1t_result result;
    switch_t1t_run(&io, &result);
    EXPECT_FALSE(raw_reads.empty());
    for (uint32_t address : raw_reads)
        EXPECT_EQ(allowed.count(address), 1u);
}

TEST(T1tReport, EveryLineFitsTheScreen)
{
    Fake fake;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_0)] = 0xffffffff;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_1)] = 0xffffffff;
    fake.value[SWITCH_T1T_I2C_REG(1, T1T_CONFIG_LOAD)] = 0xffffffff;
    for (const auto &line : screen(run(fake)))
        EXPECT_LE(line.size(), (size_t)SWITCH_DIAG_LINE_MAX);
}

TEST(T1tReport, RawWordsAndFieldsAreShown)
{
    Fake fake;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_0)] = 0x00000204;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_1)] = 0x07071b3a;
    const auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "TIMING_0 00000204 TLOW 4 THIGH 2"));
    EXPECT_TRUE(has(lines, "TIMING_1 07071B3A TSU_STA 58 THD_STA 27 TSU_STO 7 TBUF 7"));
}

TEST(T1tReport, BitsOutsideTheFieldsAreShownNotHidden)
{
    Fake fake;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_0)] = 0x80004004;
    const auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "TIMING_0 80004004 TLOW 4 THIGH 0 OTHER BITS 80004000"));
}

TEST(T1tReport, PllpFieldsAreSplitAndNoFrequencyIsClaimed)
{
    Fake fake;
    fake.value[T1T_CAR_PLLP_BASE] = 0x48115408;
    const auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "PLLP_BASE 48115408 M 8 N 85 P 1"));
    EXPECT_TRUE(has(lines, "NO FREQUENCY CLAIMED"));
    EXPECT_FALSE(has(lines, "MHZ"));
}

TEST(T1tReport, ConfigLoadBitsAreSplit)
{
    Fake fake;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_CONFIG_LOAD)] = 0x00000004;
    const auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "LOAD 00000004 BIT0 0 BIT2 1 BIT5 0 OTHER 00000000"));
}

TEST(T1tReport, ABlockThatWasNotReadSaysWhy)
{
    Fake fake;
    fake.enb_l = 0;
    fake.rst_h = CLK_I2C5;
    const auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "I2C1 NOT READ: CLOCK OFF"));
    EXPECT_TRUE(has(lines, "I2C5 NOT READ: IN RESET"));
    EXPECT_TRUE(has(lines, "COMPARISON NOT POSSIBLE"));
    EXPECT_FALSE(has(lines, "I2C1 TIMING_0"));
}

TEST(T1tReport, TheComparisonSaysEqualOrDifferentAndNeverProof)
{
    Fake fake;
    auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "TIMING_0 DIFFERENT"));
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_0)] = fake.value[SWITCH_T1T_I2C_REG(1, T1T_TIMING_0)] = 0x204;
    fake.value[SWITCH_T1T_I2C_REG(0, T1T_TIMING_1)] = fake.value[SWITCH_T1T_I2C_REG(1, T1T_TIMING_1)] = 0;
    lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "TIMING_0 EQUAL ON BOTH. TIMING_1 EQUAL ON BOTH"));
    EXPECT_TRUE(has(lines, "EQUAL WORDS ARE NOT PROOF OF A RESET VALUE"));
}

TEST(T1tReport, TheFooterStatesReadOnlyAndNoSafetyClaim)
{
    Fake fake;
    const auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "T1T COMPLETE CODE 0000. READ ONLY. NO I2C TRANSFER. NO REGISTER WRITE"));
    EXPECT_TRUE(has(lines, "REGISTER READS 25. VALUES ARE CANDIDATES FOR REVIEW, NOT A SAFETY PROOF"));
    EXPECT_FALSE(has(lines, "SAFE."));
    EXPECT_FALSE(has(lines, "PASS"));
}

TEST(T1tReport, AStoppedRunIsRed)
{
    switch_t1t_result result = {};
    result.stop = SWITCH_T1T_DENIED;
    switch_t1t_line lines[SWITCH_T1T_LINES];
    switch_t1t_lines(&result, lines);
    EXPECT_TRUE(lines[14].bad);
    EXPECT_TRUE(text(lines[14]).find("T1T STOPPED CODE 0701") != std::string::npos);
}

TEST(T1tReport, ClocksAndResetsAreShownRaw)
{
    Fake fake;
    fake.rst_l = 0x01020304;
    fake.enb_h = 0x0a0b8c0d;
    const auto lines = screen(run(fake));
    EXPECT_TRUE(has(lines, "RST_L 01020304"));
    EXPECT_TRUE(has(lines, "ENB_H 0A0B8C0D"));
}
