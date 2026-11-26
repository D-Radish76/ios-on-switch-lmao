// T2's sensor sequence against a model of the TMP451: shutdown, the one-shot with a configurable conversion time, BUSY.
#include <gtest/gtest.h>

#include <map>
#include <vector>

extern "C" {
#include "../../t2/t2.h"
}

namespace {
struct Sensor {
    uint32_t now = 0;
    uint32_t conversion_us = 34000;
    bool shutdown = true;
    bool converting = false;
    uint32_t started = 0;
    bool busy_visible = true;
    uint8_t id = 0x55, config = 0xc0;
    uint8_t remote_high = 40, remote_low = 0x80, local_high = 30, local_low = 0x40, status_extra = 0;
    std::vector<unsigned> reads;
    std::vector<std::pair<unsigned, unsigned>> writes;
    std::vector<unsigned> reads_after_stop;
    bool fail_write = false, fail_read_at = false;
    unsigned read_index = 0, fail_read_index = ~0u;
    bool change_high_between_reads = false;
    unsigned remote_high_reads = 0;

    bool busy() const { return converting && busy_visible && now - started < conversion_us; }
};

int bus_read(void *c, unsigned reg, uint8_t *v)
{
    auto *s = static_cast<Sensor *>(c);
    if (s->read_index++ == s->fail_read_index)
        return 1;
    s->reads.push_back(reg);
    switch (reg) {
    case T2_MANUFACTURER: *v = s->id; break;
    case T2_CONFIG: *v = s->config; break;
    case T2_STATUS: *v = (uint8_t)((s->busy() ? 0x80 : 0) | s->status_extra); break;
    case T2_REMOTE_HIGH:
        *v = (uint8_t)(s->remote_high + (s->change_high_between_reads && ++s->remote_high_reads == 3 ? 1 : 0));
        break;
    case T2_REMOTE_LOW: *v = s->remote_low; break;
    case T2_LOCAL_HIGH: *v = s->local_high; break;
    case T2_LOCAL_LOW: *v = s->local_low; break;
    default: *v = 0; break;
    }
    return 0;
}

int bus_write(void *c, unsigned reg, uint8_t v)
{
    auto *s = static_cast<Sensor *>(c);
    s->writes.push_back({reg, v});
    if (s->fail_write)
        return 1;
    s->converting = true;
    s->started = s->now;
    return 0;
}

uint32_t bus_now(void *c) { return static_cast<Sensor *>(c)->now; }
void bus_wait(void *c, uint32_t us) { static_cast<Sensor *>(c)->now += us; }

switch_t2_result run(Sensor &s)
{
    const switch_t2_bus bus = {&s, bus_read, bus_write, bus_now, bus_wait};
    switch_t2_result r;
    switch_t2_run(&bus, &r);
    return r;
}
} // namespace

TEST(T2, ANormalRunWritesTheOneShotOnceAndReadsTheNewValues)
{
    Sensor s;
    const auto r = run(s);
    EXPECT_EQ(r.stop, T2_STOP_NONE);
    ASSERT_EQ(s.writes.size(), 1u);
    EXPECT_EQ(s.writes[0].first, 0x0fu);
    EXPECT_EQ(s.writes[0].second, 0u);
    EXPECT_EQ(r.writes, 1u);
    EXPECT_TRUE(r.busy_seen);
    EXPECT_GE(r.done_us, 34000u);
    EXPECT_EQ(r.new_sample.remote_high, 40);
    EXPECT_FALSE(r.inconsistent);
    EXPECT_FALSE(r.not_in_shutdown);
}

TEST(T2, TheOnlyWriteIsRegisterZeroFAndNothingIsWrittenBeforeTheChecksPass)
{
    for (int fault = 0; fault < 3; ++fault) {
        Sensor s;
        if (fault == 0) s.id = 0x41;
        if (fault == 1) s.config = 0x80;
        if (fault == 2) { s.converting = true; s.started = 0; s.conversion_us = 1000000; }
        const auto r = run(s);
        EXPECT_NE(r.stop, T2_STOP_NONE) << fault;
        EXPECT_TRUE(s.writes.empty()) << fault;
        EXPECT_EQ(r.writes, 0u);
    }
}

TEST(T2, EachEntryCheckHasItsOwnStop)
{
    Sensor a; a.id = 0;
    EXPECT_EQ(run(a).stop, T2_STOP_ID);
    Sensor b; b.config = 0x00;
    EXPECT_EQ(run(b).stop, T2_STOP_CONFIG);
    Sensor c; c.converting = true; c.conversion_us = 1u << 30;
    EXPECT_EQ(run(c).stop, T2_STOP_BUSY_BEFORE);
}

TEST(T2, ACompletionBeforeThe31MsMinimumIsNotAccepted)
{
    Sensor s;
    s.conversion_us = 5000;
    const auto r = run(s);
    EXPECT_EQ(r.stop, T2_STOP_NONE);
    EXPECT_GE(r.done_us, 31000u);
}

TEST(T2, ABusyThatIsNeverSeenIsNoFailure)
{
    Sensor s;
    s.busy_visible = false;
    const auto r = run(s);
    EXPECT_EQ(r.stop, T2_STOP_NONE);
    EXPECT_FALSE(r.busy_seen);
    EXPECT_GE(r.done_us, 31000u);
}

TEST(T2, ABusyThatNeverEndsStopsAtTheBoundWithNoLaterReads)
{
    Sensor s;
    s.conversion_us = 1u << 30;
    const auto r = run(s);
    EXPECT_EQ(r.stop, T2_STOP_TIMEOUT);
    EXPECT_LT(s.now, 100000u + 6000u);
    EXPECT_GE(s.now, 100000u);
    EXPECT_EQ(s.reads.back(), (unsigned)T2_STATUS);
    EXPECT_EQ(r.new_sample.remote_high, 0);
    EXPECT_EQ(s.writes.size(), 1u);
}

TEST(T2, AFailedOneShotWriteStopsWithoutRetry)
{
    Sensor s;
    s.fail_write = true;
    const auto r = run(s);
    EXPECT_EQ(r.stop, T2_STOP_WRITE_FAILED);
    EXPECT_EQ(s.writes.size(), 1u);
    EXPECT_EQ(s.reads.back(), (unsigned)T2_LOCAL_LOW);
}

TEST(T2, AFailedReadAnywhereStopsTheRunAndNothingMoreIsRead)
{
    for (unsigned at = 0; at < 24; ++at) {
        Sensor s;
        s.fail_read_index = at;
        const auto r = run(s);
        if (r.stop == T2_STOP_NONE)
            continue;
        EXPECT_EQ(r.stop, T2_STOP_READ_FAILED) << at;
        EXPECT_LE(s.writes.size(), 1u);
        EXPECT_EQ(s.read_index, at + 1) << at;
    }
}

TEST(T2, TwoDifferentHighReadsAreInconsistent)
{
    Sensor s;
    s.change_high_between_reads = true;
    EXPECT_TRUE(run(s).inconsistent);
}

TEST(T2, AClearedShutdownBitIsShownAndNothingRestoresIt)
{
    Sensor s;
    const switch_t2_bus bus = {&s, [](void *c, unsigned reg, uint8_t *v) {
                                   const int rc = bus_read(c, reg, v);
                                   auto *x = static_cast<Sensor *>(c);
                                   if (x->converting && reg == T2_CONFIG)
                                       *v = 0x80;
                                   return rc;
                               }, bus_write, bus_now, bus_wait};
    switch_t2_result r;
    switch_t2_run(&bus, &r);
    EXPECT_TRUE(r.not_in_shutdown);
    EXPECT_EQ(s.writes.size(), 1u);
}

TEST(T2, ThePollsFollowTheDesign)
{
    Sensor s;
    run(s);
    unsigned status_reads = 0;
    for (unsigned reg : s.reads)
        status_reads += reg == T2_STATUS;
    EXPECT_LE(status_reads, 1u + 1u + 1u + 2u + 1u);
    EXPECT_GE(status_reads, 3u);
}

TEST(T2, DecodingUsesTheHighByteAndTheLowNibble)
{
    EXPECT_EQ(switch_t2_sixteenths(40, 0x80), 40 * 16 + 8);
    EXPECT_EQ(switch_t2_sixteenths(0, 0xf0), 15);
    EXPECT_TRUE(switch_t2_at_range_edge(0));
    EXPECT_TRUE(switch_t2_at_range_edge(127));
    EXPECT_FALSE(switch_t2_at_range_edge(35));
}
