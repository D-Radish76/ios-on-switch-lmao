#include "t2_bus.h"

#define STATUS_BUSY  0x100u
#define STATUS_NOACK 0xfu
#define LOAD_PENDING 1u
#define SENSOR_WRITE 0x98u
#define SENSOR_READ  0x99u

enum { CNFG_WRITE1 = 0x2800u, CNFG_READ1 = 0x2840u, CNFG_WRITE2 = 0x2802u, GO = 0x200u };

void switch_t2_hw_stop(struct switch_t2_hw *hw, int code)
{
    if (!hw->stop)
        hw->stop = code;
}

uint32_t switch_t2_hw_get(struct switch_t2_hw *hw, unsigned reg)
{
    if (!switch_t2_may_read(reg)) {
        switch_t2_hw_stop(hw, SWITCH_T2_DENIED);
        return 0;
    }
    return hw->io->read(hw->io->context, reg);
}

/* Once the run has a stop code nothing is written, whatever the caller does next. */
void switch_t2_hw_put(struct switch_t2_hw *hw, unsigned reg, uint32_t value)
{
    if (hw->stop)
        return;
    if (!switch_t2_may_write(reg, value)) {
        switch_t2_hw_stop(hw, SWITCH_T2_DENIED);
        return;
    }
    hw->io->write(hw->io->context, reg, value);
    hw->writes++;
}

uint64_t switch_t2_hw_now(struct switch_t2_hw *hw)
{
    return hw->io->now_us(hw->io->context);
}

/* Waits us microseconds. Ends by count too, so a stopped counter cannot hold the loader. */
int switch_t2_hw_wait_us(struct switch_t2_hw *hw, uint32_t us)
{
    const uint64_t start = switch_t2_hw_now(hw);
    for (unsigned polls = 0; switch_t2_hw_now(hw) - start < us; ++polls) {
        if (polls >= SWITCH_T2_POLL_LIMIT) {
            switch_t2_hw_stop(hw, T2_BUS_COUNTER_STALLED);
            return 0;
        }
    }
    return 1;
}

/* Bit 0 clears by itself when the load is done; the register as a whole does not read 0 at rest on this silicon. */
int switch_t2_hw_load(struct switch_t2_hw *hw, uint32_t value, int load_stop)
{
    switch_t2_hw_put(hw, T2_HW_I2C1_CONFIG_LOAD, value);
    if (hw->stop)
        return 0;
    const uint64_t start = switch_t2_hw_now(hw);
    for (unsigned polls = 0; switch_t2_hw_get(hw, T2_HW_I2C1_CONFIG_LOAD) & LOAD_PENDING; ++polls) {
        if (hw->stop)
            return 0;
        if (polls >= SWITCH_T2_POLL_LIMIT || switch_t2_hw_now(hw) - start > SWITCH_T2_LOAD_TIMEOUT_US) {
            switch_t2_hw_stop(hw, load_stop);
            return 0;
        }
    }
    return !hw->stop;
}

static int load_transaction(struct switch_t2_hw *hw)
{
    if (!switch_t2_hw_load(hw, SWITCH_T2_LOAD_TRANSFER, T2_BUS_LOAD_TIMEOUT))
        return 0;
    if (switch_t2_hw_get(hw, T2_HW_I2C1_CONFIG_LOAD) != SWITCH_T2_LOAD_REST)
        switch_t2_hw_stop(hw, T2_BUS_LOAD_UNEXPECTED);
    return !hw->stop;
}

/* BUSY must rise after go and then clear: a transfer that never shows BUSY is not known to have started. */
static int go_and_wait(struct switch_t2_hw *hw, uint32_t cnfg)
{
    switch_t2_hw_put(hw, T2_HW_I2C1_CNFG, cnfg | GO);
    const uint64_t start = switch_t2_hw_now(hw);
    if (hw->stop)
        return 0;
    unsigned polls = 0;
    while (!(switch_t2_hw_get(hw, T2_HW_I2C1_STATUS) & STATUS_BUSY)) {
        if (hw->stop)
            return 0;
        if (++polls >= SWITCH_T2_POLL_LIMIT || switch_t2_hw_now(hw) - start > SWITCH_T2_START_WINDOW_US) {
            switch_t2_hw_stop(hw, T2_BUS_NOT_STARTED);
            return 0;
        }
    }
    for (polls = 0; switch_t2_hw_get(hw, T2_HW_I2C1_STATUS) & STATUS_BUSY; ++polls) {
        if (hw->stop)
            return 0;
        if (polls >= SWITCH_T2_POLL_LIMIT || switch_t2_hw_now(hw) - start > SWITCH_T2_BUSY_TIMEOUT_US) {
            switch_t2_hw_stop(hw, T2_BUS_BUSY_TIMEOUT);
            return 0;
        }
    }
    if (switch_t2_hw_get(hw, T2_HW_I2C1_STATUS) & STATUS_NOACK) {
        switch_t2_hw_stop(hw, T2_BUS_NOACK);
        return 0;
    }
    hw->transfers++;
    return !hw->stop;
}

static int transaction(struct switch_t2_hw *hw, uint32_t address, uint32_t data, uint32_t cnfg)
{
    switch_t2_hw_put(hw, T2_HW_I2C1_CMD_ADDR0, address);
    if (address == SENSOR_WRITE)
        switch_t2_hw_put(hw, T2_HW_I2C1_CMD_DATA1, data);
    switch_t2_hw_put(hw, T2_HW_I2C1_CNFG, cnfg);
    return !hw->stop && load_transaction(hw) && go_and_wait(hw, cnfg);
}

static int bus_read(void *context, unsigned reg, uint8_t *value)
{
    struct switch_t2_hw *hw = context;
    if (!transaction(hw, SENSOR_WRITE, reg, CNFG_WRITE1) || !transaction(hw, SENSOR_READ, 0, CNFG_READ1))
        return 1;
    *value = (uint8_t)(switch_t2_hw_get(hw, T2_HW_I2C1_CMD_DATA1) & 0xffu);
    return hw->stop != 0;
}

/* Pointer and value share the one DATA1 word, pointer in the low byte. Only the one-shot (pointer 0F, value 00) is allowed. */
static int bus_write(void *context, unsigned reg, uint8_t value)
{
    struct switch_t2_hw *hw = context;
    if (reg != T2_ONE_SHOT || value != 0) {
        switch_t2_hw_stop(hw, T2_BUS_DENIED);
        return 1;
    }
    return !transaction(hw, SENSOR_WRITE, (uint32_t)reg | (uint32_t)value << 8, CNFG_WRITE2);
}

static uint32_t bus_now_us(void *context)
{
    return (uint32_t)switch_t2_hw_now(context);
}

static void bus_wait_us(void *context, uint32_t us)
{
    (void)switch_t2_hw_wait_us(context, us);
}

void switch_t2_hw_bus(struct switch_t2_hw *hw, struct switch_t2_bus *bus)
{
    bus->context = hw;
    bus->read = bus_read;
    bus->write = bus_write;
    bus->now_us = bus_now_us;
    bus->wait_us = bus_wait_us;
}
