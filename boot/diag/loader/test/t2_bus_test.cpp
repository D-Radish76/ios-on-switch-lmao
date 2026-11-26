// The T2 image's logic (prelude, bus layer, sensor sequence) against a model of the CAR words, the I2C1 controller and a TMP451.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <vector>

extern "C" {
#include "../t2_image.h"
#include "../t2_report.h"
}

namespace {
struct Model {
    uint64_t clock = 1000;
    uint32_t car_rst = 0, car_enb = 1u << 12, car_src = 3, car_pllp = 8u | 85u << 10 | 1u << 20;
    uint32_t addr0 = 0, data1 = 0, cnfg = 0, load = 0x4, divisor = 0x50001, timing0 = 0x204, timing1 = 0x04070404;
    uint32_t int_status = 0, idle_status = 0;
    unsigned status_polls_left = 0;
    bool noack = false, never_busy = false, load_sticks = false, busy_when_idle = false, divisor_sticks = false;
    uint64_t conversion_done_at = 0;
    std::map<unsigned, uint8_t> sensor = {{0xfe, 0x55}, {0x03, 0xc0}, {0x11, 0}, {0x12, 0}, {0x02, 0}, {0x01, 41},
                                          {0x10, 0x80}, {0x00, 31}, {0x15, 0x40}};
    std::vector<std::pair<unsigned, uint32_t>> writes;
    std::vector<unsigned> sensor_writes;
    std::set<unsigned> regs_written;
    unsigned pointer = 0;
    unsigned device_reads = 0;
};

uint32_t io_read(void *c, unsigned reg)
{
    auto *m = static_cast<Model *>(c);
    m->clock += 3;
    ++m->device_reads;
    switch (reg) {
    case T2_HW_CAR_RST_DEVICES_L: return m->car_rst;
    case T2_HW_CAR_CLK_OUT_ENB_L: return m->car_enb;
    case T2_HW_CAR_CLK_SOURCE_I2C1: return m->car_src;
    case T2_HW_CAR_PLLP_BASE: return m->car_pllp;
    case T2_HW_I2C1_STATUS: {
        const uint32_t v = (m->status_polls_left ? 0x100u : 0u) | (m->busy_when_idle ? 0x100u : 0u) | (m->noack ? 1u : 0u);
        if (m->status_polls_left)
            --m->status_polls_left;
        return v;
    }
    case T2_HW_I2C1_INT_STATUS: return m->int_status;
    case T2_HW_I2C1_CONFIG_LOAD: return m->load_sticks ? 0x5u : m->load;
    case T2_HW_I2C1_CMD_DATA1: return m->data1;
    case T2_HW_I2C1_CNFG: return m->cnfg;
    case T2_HW_I2C1_CLK_DIVISOR: return m->divisor;
    case T2_HW_I2C1_TIMING_0: return m->timing0;
    case T2_HW_I2C1_TIMING_1: return m->timing1;
    default: return 0xdeadbeef;
    }
}

void io_write(void *c, unsigned reg, uint32_t v)
{
    auto *m = static_cast<Model *>(c);
    m->writes.push_back({reg, v});
    m->regs_written.insert(reg);
    switch (reg) {
    case T2_HW_I2C1_CLK_DIVISOR:
        if (!m->divisor_sticks)
            m->divisor = v;
        break;
    case T2_HW_I2C1_CMD_ADDR0: m->addr0 = v; break;
    case T2_HW_I2C1_CMD_DATA1: m->data1 = v; break;
    case T2_HW_I2C1_CNFG:
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
    switch_t2_io io{&model, SWITCH_T2_TIMING_0_PIN, SWITCH_T2_TIMING_1_PIN, io_read, io_write, io_now};
    switch_t2_hw hw{&io, 0, 0, 0};
    switch_t2_bus bus;
    Rig() { switch_t2_hw_bus(&hw, &bus); }
};
}  // namespace

TEST(T2Image, AFullRunMakesOneSensorWriteTheDivisorWriteAndNothingElse)
{
    Rig r;
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    EXPECT_EQ(result.stop, 0);
    EXPECT_TRUE(result.sensor_ran);
    EXPECT_EQ(r.model.sensor_writes.size(), 1u);
    EXPECT_EQ(r.model.sensor_writes[0], 0u);
    EXPECT_EQ(result.sensor.writes, 1u);
    EXPECT_EQ(r.model.divisor, 0x00ff0001u);
    const std::set<unsigned> allowed = {T2_HW_I2C1_CLK_DIVISOR, T2_HW_I2C1_CNFG, T2_HW_I2C1_CMD_ADDR0, T2_HW_I2C1_CMD_DATA1,
                                        T2_HW_I2C1_CONFIG_LOAD};
    EXPECT_EQ(r.model.regs_written, allowed);
    unsigned divisor_writes = 0;
    for (const auto &w : r.model.writes)
        divisor_writes += w.first == T2_HW_I2C1_CLK_DIVISOR;
    EXPECT_EQ(divisor_writes, 1u);
    EXPECT_GE(result.transfers, 30u);
    EXPECT_GE(result.sensor.done_us, 31000u);
    EXPECT_LT(result.sensor.done_us, 100000u);
}

TEST(T2Image, AFullRunWritesTwoPlusNinePerReadPlusFiveForTheOneShot)
{
    Rig r;
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    ASSERT_EQ(result.stop, 0);
    const unsigned reads = (result.transfers - 1) / 2;
    EXPECT_EQ(result.transfers, 2 * reads + 1);
    EXPECT_EQ(r.model.writes.size(), 2u + 9u * reads + 5u);
    EXPECT_EQ(result.writes, r.model.writes.size());
    const unsigned polls = reads - 15;
    EXPECT_GE(polls, 2u);
    EXPECT_EQ(r.model.writes.size(), 142u + 9u * polls);
}

TEST(T2Image, NoCarRegisterIsEverWritten)
{
    Rig r;
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    for (const auto &w : r.model.writes)
        EXPECT_GT(w.first, (unsigned)T2_HW_CAR_PLLP_BASE);
}

TEST(T2Image, ThePreludeStopsBeforeAnyWriteOnEveryUnexpectedWord)
{
    struct Case {
        const char *name;
        void (*apply)(Model &);
        int code;
    };
    const Case cases[] = {
        {"clock off", [](Model &m) { m.car_enb = 0; }, SWITCH_T2_I2C_CLOCK_OFF},
        {"in reset", [](Model &m) { m.car_rst = 1u << 12; }, SWITCH_T2_I2C_IN_RESET},
        {"source", [](Model &m) { m.car_src = 4; }, SWITCH_T2_SOURCE_UNEXPECTED},
        {"pllp", [](Model &m) { m.car_pllp = 0; }, SWITCH_T2_PLLP_UNEXPECTED},
        {"busy", [](Model &m) { m.busy_when_idle = true; }, SWITCH_T2_BUS_BUSY},
        {"int", [](Model &m) { m.int_status = 1; }, SWITCH_T2_INT_PENDING},
        {"divisor", [](Model &m) { m.divisor = 0x00ff0001; }, SWITCH_T2_DIVISOR_UNEXPECTED},
        {"load pending", [](Model &m) { m.load = 5; }, SWITCH_T2_LOAD_BEFORE_PENDING},
        {"load word", [](Model &m) { m.load = 0; }, SWITCH_T2_LOAD_BEFORE_UNEXPECTED},
        {"timing 0", [](Model &m) { m.timing0 = 0; }, SWITCH_T2_TIMING_ZERO},
        {"timing unreadable", [](Model &m) { m.timing1 = 0xffffffff; }, SWITCH_T2_TIMING_UNREADABLE},
        {"timing different", [](Model &m) { m.timing0 = 0x205; }, SWITCH_T2_TIMING_UNEXPECTED},
    };
    for (const auto &c : cases) {
        Rig r;
        c.apply(r.model);
        switch_t2_image_result result;
        switch_t2_run_with(&r.io, &result);
        EXPECT_EQ(result.stop, c.code) << c.name;
        EXPECT_TRUE(r.model.writes.empty()) << c.name;
        EXPECT_FALSE(result.sensor_ran) << c.name;
    }
}

TEST(T2Image, ADivisorThatDoesNotStickStopsBeforeATransfer)
{
    Rig r;
    r.model.divisor_sticks = true;
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    EXPECT_EQ(result.stop, SWITCH_T2_DIVISOR_READBACK);
    EXPECT_TRUE(r.model.sensor_writes.empty());
    EXPECT_FALSE(result.sensor_ran);
}

TEST(T2Image, AWrongManufacturerIdOrConfigurationEndsBeforeTheSensorWrite)
{
    {
        Rig r;
        r.model.sensor[0xfe] = 0x54;
        switch_t2_image_result result;
        switch_t2_run_with(&r.io, &result);
        EXPECT_EQ(result.stop, T2_STOP_ID);
        EXPECT_TRUE(r.model.sensor_writes.empty());
    }
    {
        Rig r;
        r.model.sensor[0x03] = 0x80;
        switch_t2_image_result result;
        switch_t2_run_with(&r.io, &result);
        EXPECT_EQ(result.stop, T2_STOP_CONFIG);
        EXPECT_TRUE(r.model.sensor_writes.empty());
    }
}

TEST(T2Image, TheAllowlistIsExactOverSixteenBits)
{
    for (unsigned v = 0; v < 0x10000; ++v) {
        const bool pointer = v == 0xfe || v == 0x03 || v == 0x11 || v == 0x12 || v == 0x02 || v == 0x01 || v == 0x10 ||
                             v == 0x00 || v == 0x15 || v == 0x0f;
        EXPECT_EQ(switch_t2_may_write(T2_HW_I2C1_CMD_DATA1, v) != 0, pointer) << v;
        const bool cnfg = v == 0x2800 || v == 0x2840 || v == 0x2a00 || v == 0x2a40 || v == 0x2802 || v == 0x2a02;
        EXPECT_EQ(switch_t2_may_write(T2_HW_I2C1_CNFG, v) != 0, cnfg) << v;
        EXPECT_EQ(switch_t2_may_write(T2_HW_I2C1_CMD_ADDR0, v) != 0, v == 0x98 || v == 0x99) << v;
        EXPECT_EQ(switch_t2_may_write(T2_HW_I2C1_CONFIG_LOAD, v) != 0, v == 0x1 || v == 0x25) << v;
    }
    EXPECT_TRUE(switch_t2_may_write(T2_HW_I2C1_CLK_DIVISOR, 0x00ff0001u));
    for (uint32_t v : {0u, 1u, 0x00050001u, 0x00ff0000u, 0x01ff0001u, 0xffffffffu})
        EXPECT_FALSE(switch_t2_may_write(T2_HW_I2C1_CLK_DIVISOR, v)) << v;
    for (unsigned reg = 0; reg < T2_HW_REG_COUNT; ++reg)
        if (reg != T2_HW_I2C1_CMD_DATA1 && reg != T2_HW_I2C1_CNFG && reg != T2_HW_I2C1_CMD_ADDR0 &&
            reg != T2_HW_I2C1_CONFIG_LOAD && reg != T2_HW_I2C1_CLK_DIVISOR)
            for (uint32_t v : {0u, 1u, 0x25u, 0x98u, 0xffu, 0x00ff0001u})
                EXPECT_FALSE(switch_t2_may_write(reg, v)) << reg;
    EXPECT_FALSE(switch_t2_may_write(T2_HW_REG_COUNT, 0x98));
    EXPECT_EQ(switch_t2_address(T2_HW_REG_COUNT), 0u);
    EXPECT_FALSE(switch_t2_may_read(T2_HW_I2C1_CMD_ADDR0));
}

TEST(T2Image, EveryRegisterAddressIsInTheCarOrI2c1Page)
{
    for (unsigned reg = 0; reg < T2_HW_REG_COUNT; ++reg) {
        const uint32_t a = switch_t2_address(reg);
        const bool car = a >= 0x60006000u && a < 0x60007000u, i2c = a >= 0x7000c000u && a < 0x7000d000u;
        EXPECT_TRUE(car || i2c) << reg;
        EXPECT_EQ(a % 4, 0u);
        EXPECT_EQ(car, reg <= T2_HW_CAR_PLLP_BASE) << reg;
    }
}

TEST(T2Image, ASensorWriteOtherThanTheOneShotIsRefusedBeforeAnyRegisterWrite)
{
    Rig r;
    EXPECT_NE(r.bus.write(r.bus.context, 0x09, 0), 0);
    EXPECT_NE(r.bus.write(r.bus.context, T2_ONE_SHOT, 1), 0);
    EXPECT_EQ(r.hw.stop, T2_BUS_DENIED);
    EXPECT_TRUE(r.model.writes.empty());
}

TEST(T2Image, AStopCodeEndsEveryWrite)
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

TEST(T2Image, AFailureOfTheBusIsTheImagesStopCode)
{
    {
        Rig r;
        r.model.never_busy = true;
        switch_t2_image_result result;
        switch_t2_run_with(&r.io, &result);
        EXPECT_EQ(result.stop, T2_BUS_NOT_STARTED);
        EXPECT_TRUE(r.model.sensor_writes.empty());
    }
    {
        Rig r;
        r.model.noack = true;
        switch_t2_image_result result;
        switch_t2_run_with(&r.io, &result);
        EXPECT_EQ(result.stop, T2_BUS_NOACK);
        EXPECT_TRUE(r.model.sensor_writes.empty());
    }
}

TEST(T2Image, AReadReturnsTheRegisterTheControllerHolds)
{
    Rig r;
    uint8_t v = 0;
    EXPECT_EQ(r.bus.read(r.bus.context, T2_MANUFACTURER, &v), 0);
    EXPECT_EQ(v, 0x55);
    EXPECT_EQ(r.bus.read(r.bus.context, T2_REMOTE_HIGH, &v), 0);
    EXPECT_EQ(v, 41);
}

TEST(T2Image, EveryStopCodeHasAScreenName)
{
    for (int code : std::vector<int>{SWITCH_T2_I2C_CLOCK_OFF, SWITCH_T2_I2C_IN_RESET, SWITCH_T2_SOURCE_UNEXPECTED, SWITCH_T2_BUS_BUSY,
                     SWITCH_T2_DIVISOR_UNEXPECTED, SWITCH_T2_TIMING_ZERO, SWITCH_T2_TIMING_UNEXPECTED,
                     SWITCH_T2_TIMING_UNREADABLE, SWITCH_T2_IDLE_FAILED, SWITCH_T2_CONFIG_LOAD_TIMEOUT,
                     SWITCH_T2_DIVISOR_READBACK, SWITCH_T2_NOT_IDLE, SWITCH_T2_DENIED, SWITCH_T2_NOT_ERISTA,
                     SWITCH_T2_COUNTER_STALLED, SWITCH_T2_INT_PENDING, SWITCH_T2_INT_CHANGED,
                     SWITCH_T2_LOAD_BEFORE_UNEXPECTED, SWITCH_T2_LOAD_BEFORE_PENDING, SWITCH_T2_LOAD_AFTER_UNEXPECTED,
                     SWITCH_T2_PLLP_UNEXPECTED, T2_STOP_READ_FAILED, T2_STOP_ID, T2_STOP_CONFIG, T2_STOP_BUSY_BEFORE,
                     T2_STOP_WRITE_FAILED, T2_STOP_TIMEOUT, T2_BUS_DENIED, T2_BUS_NOT_STARTED, T2_BUS_BUSY_TIMEOUT,
                     T2_BUS_NOACK, T2_BUS_LOAD_TIMEOUT, T2_BUS_LOAD_UNEXPECTED, T2_BUS_COUNTER_STALLED})
        EXPECT_STRNE(switch_t2_stop_name(code), "UNKNOWN") << code;
}

TEST(T2Image, TheScreenShowsAReadingOnlyFromACompletedRunAndNoLineIsCut)
{
    Rig r;
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    switch_t2_line lines[SWITCH_T2_LINES];
    switch_t2_lines(&result, lines);
    std::string all;
    for (const auto &l : lines) {
        EXPECT_LT(l.text.length, SWITCH_DIAG_LINE_MAX) << l.text.text;
        all += std::string(l.text.text) + "\n";
    }
    EXPECT_NE(all.find("REMOTE 41.5000 C, LOCAL 31.2500 C"), std::string::npos) << all;
    EXPECT_NE(all.find("NOT A LIMIT AND NOT A SAFETY CRITERION"), std::string::npos);
    EXPECT_NE(all.find("T2 COMPLETE CODE 0000"), std::string::npos);

    Rig stopped;
    stopped.model.sensor[0xfe] = 0;
    switch_t2_image_result failed;
    switch_t2_run_with(&stopped.io, &failed);
    switch_t2_lines(&failed, lines);
    all.clear();
    for (const auto &l : lines)
        all += std::string(l.text.text) + "\n";
    EXPECT_EQ(all.find(" C, LOCAL "), std::string::npos) << all;
    EXPECT_NE(all.find("T2 STOPPED CODE 0721"), std::string::npos);
}

TEST(T2Image, AnOpenDiodeOrARangeEdgeIsNotShownAsATemperature)
{
    Rig r;
    r.model.sensor[0x01] = 127;
    switch_t2_image_result result;
    switch_t2_run_with(&r.io, &result);
    switch_t2_line lines[SWITCH_T2_LINES];
    switch_t2_lines(&result, lines);
    std::string all;
    for (const auto &l : lines)
        all += std::string(l.text.text) + "\n";
    EXPECT_NE(all.find("NOT A READING"), std::string::npos) << all;
    EXPECT_EQ(all.find(" C, LOCAL "), std::string::npos);
}
