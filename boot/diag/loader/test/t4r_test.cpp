// T4-R's read logic on the host: the allowlist, the order of the reads, the state classification and the screen lines.
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../t4r.h"
#include "../t4r_report.h"
}

namespace {
struct Fake {
    uint32_t tmrus_first = 1000000, tmrus_second = 1200000, ptv = 0, config = 0, status = 0;
    std::vector<std::string> events; // "show N" and "read N" in the order they happened
    int refuse_step = -1;
    unsigned tmrus_reads = 0;

    static int show(void *context, unsigned step, const switch_t4r_result *)
    {
        Fake *self = static_cast<Fake *>(context);
        self->events.push_back("show " + std::to_string(step));
        return static_cast<int>(step) != self->refuse_step;
    }

    static uint32_t read(void *context, unsigned reg)
    {
        Fake *self = static_cast<Fake *>(context);
        self->events.push_back("read " + std::to_string(reg));
        switch (reg) {
        case T4R_TMRUS: return self->tmrus_reads++ ? self->tmrus_second : self->tmrus_first;
        case T4R_TMR5_PTV: return self->ptv;
        case T4R_WDT0_CONFIG: return self->config;
        case T4R_WDT0_STATUS: return self->status;
        }
        return 0xdead;
    }

    switch_t4r_result run()
    {
        switch_t4r_io io{this, read, show};
        switch_t4r_result result;
        switch_t4r_run(&io, &result);
        return result;
    }

    unsigned reads() const
    {
        unsigned n = 0;
        for (const auto &e : events)
            n += e.rfind("read", 0) == 0;
        return n;
    }
};
}

TEST(T4R, FiveReadsInOrderEachBetweenItsBeginAndResult)
{
    Fake fake;
    const auto result = fake.run();
    EXPECT_EQ(result.stop, 0);
    EXPECT_EQ(result.reads, 5u);
    std::vector<std::string> expected{"show 0"};
    const unsigned regs[] = {T4R_TMRUS, T4R_TMR5_PTV, T4R_WDT0_CONFIG, T4R_WDT0_STATUS};
    for (unsigned i = 0; i < 4; ++i) {
        expected.push_back("show " + std::to_string(1 + 2 * i));
        expected.push_back("read " + std::to_string(regs[i]));
        expected.push_back("show " + std::to_string(2 + 2 * i));
    }
    expected.push_back("show " + std::to_string(T4R_STEP_STATE));
    expected.push_back("show " + std::to_string(T4R_STEP_FIELDS));
    expected.push_back("show " + std::to_string(T4R_STEP_TMRUS2_BEGIN));
    expected.push_back("read " + std::to_string(T4R_TMRUS));
    expected.push_back("show " + std::to_string(T4R_STEP_TMRUS2_RESULT));
    expected.push_back("show " + std::to_string(T4R_STEP_COMPLETE));
    EXPECT_EQ(fake.events, expected);
    EXPECT_TRUE(result.tmrus_advanced);
    EXPECT_EQ(result.state, T4R_STATE_IDLE);
}

TEST(T4R, AShownStepThatDoesNotSettleStopsBeforeTheNextRead)
{
    for (unsigned step = 0; step < T4R_STEP_COMPLETE; ++step) {
        Fake fake;
        fake.refuse_step = static_cast<int>(step);
        const auto result = fake.run();
        EXPECT_EQ(result.stop, SWITCH_T4R_NOT_SHOWN) << step;
        for (size_t i = 0; i < fake.events.size(); ++i)
            if (fake.events[i] == "show " + std::to_string(step))
                for (size_t j = i + 1; j < fake.events.size(); ++j)
                    EXPECT_NE(fake.events[j].rfind("read", 0), 0u) << "step " << step << " event " << fake.events[j];
    }
}

TEST(T4R, AllowlistIsFourWordAddressesAtThirtyTwoBitsAndNothingElse)
{
    std::set<uint32_t> addresses;
    for (unsigned reg = 0; reg < 64; ++reg)
        for (unsigned width = 0; width <= 64; ++width) {
            const bool allowed = switch_t4r_may_read(reg, width);
            EXPECT_EQ(allowed, reg < T4R_REG_COUNT && width == 32u) << reg << " " << width;
            if (allowed)
                addresses.insert(switch_t4r_address(reg));
        }
    for (unsigned reg = T4R_REG_COUNT; reg < 64; ++reg)
        EXPECT_EQ(switch_t4r_address(reg), 0u);
    EXPECT_EQ(addresses, (std::set<uint32_t>{0x60005010u, 0x60005060u, 0x60005100u, 0x60005104u}));
}

TEST(T4R, ClassificationFollowsOnlyWhatTheSourcesNameAndNothingIsSet)
{
    struct Row { uint32_t ptv, config, status; unsigned state, why; };
    const Row rows[] = {
        {0, 0, 0, T4R_STATE_IDLE, T4R_WHY_NONE},
        {0, 0x8005 | 0x30, 0, T4R_STATE_CONFIGURED_STOPPED, T4R_WHY_NONE},
        {0, 0x8035, 1, T4R_STATE_DORMANT, T4R_WHY_NONE},
        {0xc003d08f, 0x8035, 1, T4R_STATE_BUSY, T4R_WHY_NONE},
        {0, 0, 2, T4R_STATE_BUSY, T4R_WHY_NONE},
        {0xc0000001u, 0, 0, T4R_STATE_BUSY, T4R_WHY_NONE},
        {0xc003d08f, 0x8035, (200u << 4) | 1u, T4R_STATE_BUSY, T4R_WHY_NONE},
        {0xc003d08f, 0x8035, (3u << 12) | (255u << 4) | 1u, T4R_STATE_BUSY, T4R_WHY_NONE},
        {0x80000000u, 0, 0, T4R_STATE_UNKNOWN, T4R_WHY_CONFLICT},
        {0xc0000000u, 0x8035, 1, T4R_STATE_UNKNOWN, T4R_WHY_CONFLICT},
        {0x1234, 0, 0, T4R_STATE_UNKNOWN, T4R_WHY_TIMER_VALUE},
        {0x40000000u, 0, 0, T4R_STATE_IDLE, T4R_WHY_NONE},
        {0, 0x8036, 1, T4R_STATE_UNKNOWN, T4R_WHY_OTHER_SOURCE},
        {0, 0, 5u << 4, T4R_STATE_UNKNOWN, T4R_WHY_COUNT_LEFT},
        {0, 0x8035, (255u << 4) | 1u, T4R_STATE_UNKNOWN, T4R_WHY_COUNT_LEFT},
        {0, 0, 1u << 12, T4R_STATE_UNKNOWN, T4R_WHY_EXPIRY_LEFT},
        {0, 0, 3u << 12, T4R_STATE_UNKNOWN, T4R_WHY_EXPIRY_LEFT},
        {0, 0, (1u << 12) | (1u << 4), T4R_STATE_UNKNOWN, T4R_WHY_COUNT_AND_EXPIRY_LEFT},
        {0, 0, 1u << 2, T4R_STATE_UNKNOWN, T4R_WHY_STATUS_BITS},
        {0, 0, 1u << 3, T4R_STATE_UNKNOWN, T4R_WHY_STATUS_BITS},
        {0, 0, 1u << 14, T4R_STATE_UNKNOWN, T4R_WHY_STATUS_BITS},
        {0, 0, 1u << 31, T4R_STATE_UNKNOWN, T4R_WHY_STATUS_BITS},
        {0xc003d08f, 0x8035, 1u << 20, T4R_STATE_UNKNOWN, T4R_WHY_STATUS_BITS},
        {0, 0, 0x8000001, T4R_STATE_UNKNOWN, T4R_WHY_STATUS_BITS},
        {0xffffffffu, 0, 0, T4R_STATE_UNKNOWN, T4R_WHY_ALL_ONES},
        {0, 0xffffffffu, 0, T4R_STATE_UNKNOWN, T4R_WHY_ALL_ONES},
        {0, 0, 0xffffffffu, T4R_STATE_UNKNOWN, T4R_WHY_ALL_ONES},
    };
    for (const Row &row : rows) {
        unsigned why = 99;
        EXPECT_EQ(switch_t4r_classify(row.ptv, row.config, row.status, &why), row.state) << row.ptv << " " << row.config << " " << row.status;
        EXPECT_EQ(why, row.why);
    }
}

TEST(T4R, TmrusAdvanceIsModuloThirtyTwoBitsAndZeroIsNotAdvance)
{
    EXPECT_TRUE(switch_t4r_advanced(10, 11));
    EXPECT_TRUE(switch_t4r_advanced(0xfffffff0u, 0x10u));
    EXPECT_FALSE(switch_t4r_advanced(5, 5));
    EXPECT_FALSE(switch_t4r_advanced(100, 99));
    Fake fake;
    fake.tmrus_second = fake.tmrus_first;
    const auto result = fake.run();
    EXPECT_FALSE(result.tmrus_advanced);
    EXPECT_EQ(result.stop, 0);
    switch_t4r_line line;
    switch_t4r_screen_line(T4R_STEP_TMRUS2_RESULT, &result, &line);
    EXPECT_TRUE(line.bad);
    EXPECT_NE(std::string(line.text.text).find("DID NOT ADVANCE"), std::string::npos);
}

TEST(T4R, AnUnknownStateStillMakesAllFiveReadsAndNothingMore)
{
    Fake fake;
    fake.status = 0xffffffffu;
    const auto result = fake.run();
    EXPECT_EQ(result.state, T4R_STATE_UNKNOWN);
    EXPECT_EQ(result.reads, 5u);
    EXPECT_EQ(fake.reads(), 5u);
}

TEST(T4R, ScreenLinesNameTheAddressAndWidthBeforeAndTheValueAfter)
{
    switch_t4r_result result{};
    result.config = 0x8035;
    switch_t4r_line line;
    switch_t4r_screen_line(T4R_STEP_CONFIG_BEGIN, &result, &line);
    EXPECT_EQ(std::string(line.text.text), "T4R WDT0 CONFIG READ BEGIN. ADDRESS 60005100. 32 BIT");
    switch_t4r_screen_line(T4R_STEP_CONFIG_RESULT, &result, &line);
    EXPECT_EQ(std::string(line.text.text), "T4R WDT0 CONFIG READ RESULT 00008035");
    result.status = (3u << 12) | (200u << 4) | 1u;
    switch_t4r_screen_line(T4R_STEP_FIELDS, &result, &line);
    EXPECT_EQ(std::string(line.text.text), "T4R WDT0 STATUS COUNT 200 EXPIRY 3. BITS 0 AND 1 NOT CONFIRMED");
    EXPECT_FALSE(line.bad);
    result.state = T4R_STATE_UNKNOWN;
    result.why = T4R_WHY_ALL_ONES;
    switch_t4r_screen_line(T4R_STEP_STATE, &result, &line);
    EXPECT_NE(std::string(line.text.text).find("AFTER HEKATE INIT"), std::string::npos);
    EXPECT_TRUE(line.bad);
}
