#include "t2_bus.h"

#define STATUS_BUSY  0x100u
#define STATUS_NOACK 0xfu
#define LOAD_PENDING 1u
#define SENSOR_WRITE 0x98u
#define SENSOR_READ  0x99u

enum { CNFG_WRITE1 = 0x2800u, CNFG_READ1 = 0x2840u, CNFG_WRITE2 = 0x2802u, GO = 0x200u };

int switch_t2_may_write(unsigned reg, uint32_t value)
{
    switch (reg) {
    case T1V3_I2C1_CMD_DATA1:
        switch (value) {
        case 0xfe: case 0x03: case 0x11: case 0x12: case 0x02: case 0x01: case 0x10: case 0x00: case 0x15: case 0x0f:
            return 1;
        default:
            return 0;
        }
    case T1V3_I2C1_CNFG:
        return value == 0x2802u || value == 0x2a02u || switch_t1v3_may_write(reg, value);
    case T1V3_I2C1_CLK_DIVISOR:
        return 0;
    default:
        return switch_t1v3_may_write(reg, value);
    }
}

static void stop(struct switch_t2_hw *hw, int code)
{
    if (!hw->stop)
        hw->stop = code;
}

static uint32_t get(struct switch_t2_hw *hw, unsigned reg)
{
    if (!switch_t1v3_may_read(reg)) {
        stop(hw, T2_BUS_DENIED);
        return 0;
    }
    return hw->io->read(hw->io->context, reg);
}

/* Once the bus has a stop code nothing is written, whatever the caller does next. */
static void put(struct switch_t2_hw *hw, unsigned reg, uint32_t value)
{
    if (hw->stop)
        return;
    if (!switch_t2_may_write(reg, value)) {
        stop(hw, T2_BUS_DENIED);
        return;
    }
    hw->io->write(hw->io->context, reg, value);
    hw->writes++;
}

static uint64_t now(struct switch_t2_hw *hw)
{
    return hw->io->now_us(hw->io->context);
}

static int load_transaction(struct switch_t2_hw *hw)
{
    put(hw, T1V3_I2C1_CONFIG_LOAD, SWITCH_T1V3_LOAD_TRANSFER);
    if (hw->stop)
        return 0;
    const uint64_t start = now(hw);
    for (unsigned polls = 0; get(hw, T1V3_I2C1_CONFIG_LOAD) & LOAD_PENDING; ++polls) {
        if (hw->stop)
            return 0;
        if (polls >= SWITCH_T1V3_POLL_LIMIT || now(hw) - start > SWITCH_T1V3_LOAD_TIMEOUT_US) {
            stop(hw, T2_BUS_LOAD_TIMEOUT);
            return 0;
        }
    }
    if (get(hw, T1V3_I2C1_CONFIG_LOAD) != SWITCH_T1V3_LOAD_REST)
        stop(hw, T2_BUS_LOAD_UNEXPECTED);
    return !hw->stop;
}

/* BUSY must rise after go and then clear, as in T1 v3: a transfer that never shows BUSY is not known to have started. */
static int go_and_wait(struct switch_t2_hw *hw, uint32_t cnfg)
{
    put(hw, T1V3_I2C1_CNFG, cnfg | GO);
    const uint64_t start = now(hw);
    if (hw->stop)
        return 0;
    unsigned polls = 0;
    while (!(get(hw, T1V3_I2C1_STATUS) & STATUS_BUSY)) {
        if (hw->stop)
            return 0;
        if (++polls >= SWITCH_T1V3_POLL_LIMIT || now(hw) - start > SWITCH_T1V3_START_WINDOW_US) {
            stop(hw, T2_BUS_NOT_STARTED);
            return 0;
        }
    }
    for (polls = 0; get(hw, T1V3_I2C1_STATUS) & STATUS_BUSY; ++polls) {
        if (hw->stop)
            return 0;
        if (polls >= SWITCH_T1V3_POLL_LIMIT || now(hw) - start > SWITCH_T1V3_BUSY_TIMEOUT_US) {
            stop(hw, T2_BUS_BUSY_TIMEOUT);
            return 0;
        }
    }
    if (get(hw, T1V3_I2C1_STATUS) & STATUS_NOACK) {
        stop(hw, T2_BUS_NOACK);
        return 0;
    }
    hw->transfers++;
    return !hw->stop;
}

static int transaction(struct switch_t2_hw *hw, uint32_t address, uint32_t data, uint32_t cnfg)
{
    put(hw, T1V3_I2C1_CMD_ADDR0, address);
    if (address == SENSOR_WRITE)
        put(hw, T1V3_I2C1_CMD_DATA1, data);
    put(hw, T1V3_I2C1_CNFG, cnfg);
    return !hw->stop && load_transaction(hw) && go_and_wait(hw, cnfg);
}

static int bus_read(void *context, unsigned reg, uint8_t *value)
{
    struct switch_t2_hw *hw = context;
    if (!transaction(hw, SENSOR_WRITE, reg, CNFG_WRITE1) || !transaction(hw, SENSOR_READ, 0, CNFG_READ1))
        return 1;
    *value = (uint8_t)(get(hw, T1V3_I2C1_CMD_DATA1) & 0xffu);
    return hw->stop != 0;
}

/* Pointer and value share the one DATA1 word, pointer in the low byte. Only the one-shot (pointer 0F, value 00) is allowed. */
static int bus_write(void *context, unsigned reg, uint8_t value)
{
    struct switch_t2_hw *hw = context;
    if (reg != T2_ONE_SHOT || value != 0) {
        stop(hw, T2_BUS_DENIED);
        return 1;
    }
    return !transaction(hw, SENSOR_WRITE, (uint32_t)reg | (uint32_t)value << 8, CNFG_WRITE2);
}

static uint32_t bus_now_us(void *context)
{
    return (uint32_t)now(context);
}

static void bus_wait_us(void *context, uint32_t us)
{
    struct switch_t2_hw *hw = context;
    const uint64_t start = now(hw);
    for (unsigned polls = 0; now(hw) - start < us; ++polls)
        if (polls >= SWITCH_T1V3_POLL_LIMIT) {
            stop(hw, T2_BUS_COUNTER_STALLED);
            return;
        }
}

void switch_t2_hw_bus(struct switch_t2_hw *hw, struct switch_t2_bus *bus)
{
    bus->context = hw;
    bus->read = bus_read;
    bus->write = bus_write;
    bus->now_us = bus_now_us;
    bus->wait_us = bus_wait_us;
}
