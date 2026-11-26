// T2's bus layer and sequence against a model of the I2C1 controller with a TMP451 behind it.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <vector>

extern "C" {
#include "../t2_bus.h"
}

namespace {
struct Model {
    uint64_t clock = 1000;
    uint32_t addr0 = 0, data1 = 0, cnfg = 0, load = 0x4;
    unsigned status_polls_left = 0;
    bool noack = false, never_busy = false, load_sticks = false, fail_denied_probe = false;
    uint64_t conversion_done_at = 0;
    bool shutdown = true;
    std::map<unsigned, uint8_t> sensor = {{0xfe, 0x55}, {0x03, 0xc0}, {0x11, 0}, {0x12, 0}, {0x02, 0}, {0x01, 41},
                                          {0x10, 0x80}, {0x00, 31}, {0x15, 0x40}};
    std::vector<std::pair<unsigned, uint32_t>> writes;
    std::vector<unsigned> sensor_writes;
    std::set<unsigned> regs_written;
    unsigned pointer = 0;

    uint32_t status() const
    {
        return (status_polls_left ? 0x100u : 0u) | (noack ? 1u : 0u);
    }
};

uint32_t io_read(void *c, unsigned reg)
{
    auto *m = static_cast<Model *>(c);
    m->clock += 3;
    switch (reg) {
    case T1V3_I2C1_STATUS: {
        const uint32_t v = m->status();
        if (m->status_polls_left)
            --m->status_polls_left;
        return v;
    }
    case T1V3_I2C1_CONFIG_LOAD: return m->load_sticks ? 0x5u : m->load;
    case T1V3_I2C1_CMD_DATA1: return m->data1;
    case T1V3_I2C1_CNFG: return m->cnfg;
    default: return 0;
    }
}

void io_write(void *c, unsigned reg, uint32_t v)
{
    auto *m = static_cast<Model *>(c);
    m->writes.push_back({reg, v});
    m->regs_written.insert(reg);
    switch (reg) {
    case T1V3_I2C1_CMD_ADDR0: m->addr0 = v; break;
    case T1V3_I2C1_CMD_DATA1: m->data1 = v; break;
    case T1V3_I2C1_CNFG:
        m->cnfg = v;
        if (!(v & 0x200))
            break;
        if (!m->never_busy)
            m->status_polls_left = 2;
        if (m->addr0 == 0x98) {
            m->pointer = m->data1 & 0xff;
            if (m->pointer == 0x0f) {
                m->sensor_writes.push_back((m->data1 >> 8) & 0xff);
                m->conversion_done_at = m->clock + 34000;
                m->sensor[0x02] = 0x80;
            }
        } else {
            m->data1 = m->sensor[m->pointer];
        }
        break;
    default: break;
    }
}

uint64_t io_now(void *c)
{
    auto *m = static_cast<Model *>(c);
    m->clock += 10;
    if (m->conversion_done_at && m->clock >= m->conversion_done_at) {
        m->sensor[0x02] = 0;
        m->conversion_done_at = 0;
    }
    return m->clock;
}

struct Rig {
    Model model;
    switch_t1v3_io io{&model, 0, 0, io_read, io_write, io_now};
    switch_t2_hw hw{&io, 0, 0, 0};
    switch_t2_bus bus;
    Rig() { switch_t2_hw_bus(&hw, &bus); }
};
} // namespace

TEST(T2Bus, AFullRunMakesExactlyOneSensorWriteAndNoOtherRegisterWrite)
{
    Rig r;
    switch_t2_result result;
    switch_t2_run(&r.bus, &result);
    EXPECT_EQ(result.stop, 0);
    EXPECT_EQ(r.hw.stop, 0);
    EXPECT_EQ(r.model.sensor_writes.size(), 1u);
    EXPECT_EQ(r.model.sensor_writes[0], 0u);
    EXPECT_EQ(result.writes, 1u);
    const std::set<unsigned> allowed = {T1V3_I2C1_CNFG, T1V3_I2C1_CMD_ADDR0, T1V3_I2C1_CMD_DATA1, T1V3_I2C1_CONFIG_LOAD};
    EXPECT_EQ(r.model.regs_written, allowed);
    EXPECT_LE(r.hw.writes, 200u);
    EXPECT_GE(r.hw.writes, 100u);
}

TEST(T2Bus, TheDivisorAndEveryCarRegisterAreNeverWritten)
{
    Rig r;
    switch_t2_result result;
    switch_t2_run(&r.bus, &result);
    for (const auto &w : r.model.writes) {
        EXPECT_NE(w.first, (unsigned)T1V3_I2C1_CLK_DIVISOR);
        EXPECT_NE(w.first, (unsigned)T1V3_I2C1_TIMING_0);
        EXPECT_NE(w.first, (unsigned)T1V3_I2C1_TIMING_1);
        EXPECT_GT(w.first, (unsigned)T1V3_CAR_PLLP_BASE);
    }
}

TEST(T2Bus, TheAllowlistIsT1V3sPlusTheNewValuesAndNothingElse)
{
    for (unsigned v = 0; v < 0x10000; ++v) {
        const bool pointer = v == 0xfe || v == 0x03 || v == 0x11 || v == 0x12 || v == 0x02 || v == 0x01 || v == 0x10 ||
                             v == 0x00 || v == 0x15 || v == 0x0f;
        EXPECT_EQ(switch_t2_may_write(T1V3_I2C1_CMD_DATA1, v) != 0, pointer) << v;
        const bool cnfg = v == 0x2800 || v == 0x2840 || v == 0x2a00 || v == 0x2a40 || v == 0x2802 || v == 0x2a02;
        EXPECT_EQ(switch_t2_may_write(T1V3_I2C1_CNFG, v) != 0, cnfg) << v;
        EXPECT_EQ(switch_t2_may_write(T1V3_I2C1_CMD_ADDR0, v) != 0, v == 0x98 || v == 0x99) << v;
        EXPECT_EQ(switch_t2_may_write(T1V3_I2C1_CONFIG_LOAD, v) != 0, v == 0x1 || v == 0x25) << v;
        EXPECT_FALSE(switch_t2_may_write(T1V3_I2C1_CLK_DIVISOR, v)) << v;
    }
    for (unsigned reg = 0; reg < T1V3_REG_COUNT; ++reg)
        if (reg != T1V3_I2C1_CMD_DATA1 && reg != T1V3_I2C1_CNFG && reg != T1V3_I2C1_CMD_ADDR0 && reg != T1V3_I2C1_CONFIG_LOAD)
            for (uint32_t v : {0u, 1u, 0x25u, 0x98u, 0xffu, 0x00ff0001u})
                EXPECT_FALSE(switch_t2_may_write(reg, v)) << reg;
}

TEST(T2Bus, ASensorWriteOtherThanTheOneShotIsRefusedBeforeAnyRegisterWrite)
{
    Rig r;
    EXPECT_NE(r.bus.write(r.bus.context, 0x09, 0), 0);
    EXPECT_NE(r.bus.write(r.bus.context, T2_ONE_SHOT, 1), 0);
    EXPECT_EQ(r.hw.stop, T2_BUS_DENIED);
    EXPECT_TRUE(r.model.writes.empty());
}

TEST(T2Bus, AStopCodeEndsEveryWrite)
{
    Rig r;
    r.model.noack = true;
    uint8_t v;
    EXPECT_NE(r.bus.read(r.bus.context, T2_MANUFACTURER, &v), 0);
    EXPECT_EQ(r.hw.stop, T2_BUS_NOACK);
    const size_t before = r.model.writes.size();
    EXPECT_NE(r.bus.read(r.bus.context, T2_CONFIG, &v), 0);
    EXPECT_EQ(r.model.writes.size(), before);
}

TEST(T2Bus, ATransferThatNeverShowsBusyStopsAsNotStarted)
{
    Rig r;
    r.model.never_busy = true;
    uint8_t v;
    EXPECT_NE(r.bus.read(r.bus.context, T2_MANUFACTURER, &v), 0);
    EXPECT_EQ(r.hw.stop, T2_BUS_NOT_STARTED);
}

TEST(T2Bus, ALoadThatNeverClearsStopsTheTransfer)
{
    Rig r;
    r.model.load_sticks = true;
    uint8_t v;
    EXPECT_NE(r.bus.read(r.bus.context, T2_MANUFACTURER, &v), 0);
    EXPECT_EQ(r.hw.stop, T2_BUS_LOAD_TIMEOUT);
}

TEST(T2Bus, AReadReturnsTheRegisterTheControllerHolds)
{
    Rig r;
    uint8_t v = 0;
    EXPECT_EQ(r.bus.read(r.bus.context, T2_MANUFACTURER, &v), 0);
    EXPECT_EQ(v, 0x55);
    EXPECT_EQ(r.bus.read(r.bus.context, T2_REMOTE_HIGH, &v), 0);
    EXPECT_EQ(v, 41);
}

TEST(T2Bus, TheConversionIsWaitedForOnTheRealCounterAndTheDoneTimeIsPastTheMinimum)
{
    Rig r;
    switch_t2_result result;
    switch_t2_run(&r.bus, &result);
    EXPECT_GE(result.done_us, (uint32_t)T2_MIN_US);
    EXPECT_LT(result.done_us, (uint32_t)T2_BOUND_US);
}
