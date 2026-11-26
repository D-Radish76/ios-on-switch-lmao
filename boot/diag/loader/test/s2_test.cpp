// D0-S v2's read logic on the host: the allowlist, the order of the reads and when SDMMC1 is touched.
#include <gtest/gtest.h>

#include <set>
#include <vector>

extern "C" {
#include "../s2.h"
}

namespace {
constexpr uint32_t SD = 1u << 14;

struct Fake {
    uint32_t rst_l = 0, enb_l = SD, present = 0x01f70000, word_2c = 0x00000107;
    std::vector<unsigned> reads;

    static uint32_t read(void *context, unsigned reg)
    {
        Fake *self = static_cast<Fake *>(context);
        self->reads.push_back(reg);
        switch (reg) {
        case S2_CAR_RST_DEVICES_L: return self->rst_l;
        case S2_CAR_CLK_OUT_ENB_L: return self->enb_l;
        case S2_SDMMC1_PRESENT_STATE: return self->present;
        case S2_SDMMC1_WORD_2C: return self->word_2c;
        }
        return 0xdead;
    }

    switch_s2_result run()
    {
        switch_s2_io io{this, read};
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
    switch_s2_io io{&fake, Fake::read};
    switch_s2_run(&io, &result);
    EXPECT_EQ(result.stop, 0);
}
