// A model of the CAR words, the I2C1 controller and a TMP451, shared by the T2 image tests.
#ifndef SWITCH_DIAG_T2_MODEL_H
#define SWITCH_DIAG_T2_MODEL_H
#include <map>
#include <set>
#include <vector>

extern "C" {
#include "../t2_image.h"
#include "../t2_report.h"
}

namespace t2model {
struct Model {
    uint64_t clock = 1000;
    uint32_t car_rst = 0, car_enb = 1u << 12, car_src = 3, car_pllp = 8u | 85u << 10 | 1u << 20;
    uint32_t addr0 = 0, data1 = 0, cnfg = 0, load = 0x4, divisor = 0x50001, timing0 = 0x204, timing1 = 0x04070404;
    uint32_t int_status = 0, idle_status = 0;
    unsigned status_polls_left = 0;
    bool noack = false, never_busy = false, load_sticks = false, busy_when_idle = false, divisor_sticks = false;
    uint64_t conversion_done_at = 0;
    uint64_t conversion_us = 34000;
    std::map<unsigned, uint8_t> after_conversion;
    std::map<unsigned, uint8_t> sensor = {{0xfe, 0x55}, {0x03, 0xc0}, {0x11, 0}, {0x12, 0}, {0x02, 0}, {0x01, 41},
                                          {0x10, 0x80}, {0x00, 31}, {0x15, 0x40}};
    std::vector<std::pair<unsigned, uint32_t>> writes;
    std::vector<unsigned> sensor_writes;
    std::set<unsigned> regs_written;
    unsigned pointer = 0;
    unsigned device_reads = 0;
};

inline uint32_t io_read(void *c, unsigned reg)
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

inline void io_write(void *c, unsigned reg, uint32_t v)
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
                m->conversion_done_at = m->clock + m->conversion_us;
                m->sensor[0x02] = 0x80;
            }
        } else {
            m->data1 = m->sensor[m->pointer];
        }
        break;
    default: break;
    }
}

inline uint64_t io_now(void *c)
{
    auto *m = static_cast<Model *>(c);
    m->clock += 10;
    if (m->conversion_done_at && m->clock >= m->conversion_done_at) {
        m->sensor[0x02] = 0;
        for (const auto &v : m->after_conversion)
            m->sensor[v.first] = v.second;
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
}  // namespace t2model
#endif
