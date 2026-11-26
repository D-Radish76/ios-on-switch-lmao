#include "t2_prelude.h"

#define CLK_L_I2C1   (1u << 12)
#define STATUS_BUSY  0x100u
#define LOAD_PENDING 1u
#define UNREADABLE   0xffffffffu

/* The counter every time limit rests on must advance before any device is touched. */
static int counter_advances(const struct switch_t2_io *io)
{
    const uint64_t first = io->now_us(io->context);
    for (unsigned i = 0; i < SWITCH_T2_POLL_LIMIT; ++i)
        if (io->now_us(io->context) != first)
            return 1;
    return 0;
}

static int read_car(struct switch_t2_hw *hw, struct switch_t2_probe *p)
{
    p->rst_devices_l = switch_t2_hw_get(hw, T2_HW_CAR_RST_DEVICES_L);
    p->clk_out_enb_l = switch_t2_hw_get(hw, T2_HW_CAR_CLK_OUT_ENB_L);
    p->source_i2c1 = switch_t2_hw_get(hw, T2_HW_CAR_CLK_SOURCE_I2C1);
    p->pllp_base = switch_t2_hw_get(hw, T2_HW_CAR_PLLP_BASE);
    if (hw->stop)
        return 0;
    if (!(p->clk_out_enb_l & CLK_L_I2C1))
        switch_t2_hw_stop(hw, SWITCH_T2_I2C_CLOCK_OFF);
    else if (p->rst_devices_l & CLK_L_I2C1)
        switch_t2_hw_stop(hw, SWITCH_T2_I2C_IN_RESET);
    else if (p->source_i2c1 != SWITCH_T2_SOURCE_EXPECTED)
        switch_t2_hw_stop(hw, SWITCH_T2_SOURCE_UNEXPECTED);
    else if (SWITCH_T2_PLLP_M(p->pllp_base) != SWITCH_T2_PLLP_M_EXPECTED ||
             SWITCH_T2_PLLP_N(p->pllp_base) != SWITCH_T2_PLLP_N_EXPECTED ||
             SWITCH_T2_PLLP_P(p->pllp_base) != SWITCH_T2_PLLP_P_EXPECTED)
        switch_t2_hw_stop(hw, SWITCH_T2_PLLP_UNEXPECTED);
    return !hw->stop;
}

/* 0 and all ones are named stops of their own. A word is accepted only when it equals its pin, and a pin of 0 never matches. */
static int timing_stop(uint32_t value, uint32_t pin)
{
    if (value == 0)
        return SWITCH_T2_TIMING_ZERO;
    if (value == UNREADABLE)
        return SWITCH_T2_TIMING_UNREADABLE;
    return value != pin ? SWITCH_T2_TIMING_UNEXPECTED : 0;
}

/* Everything here is a read. */
static int read_controller(struct switch_t2_hw *hw, struct switch_t2_probe *p)
{
    p->cnfg = switch_t2_hw_get(hw, T2_HW_I2C1_CNFG);
    p->status = switch_t2_hw_get(hw, T2_HW_I2C1_STATUS);
    p->int_status = switch_t2_hw_get(hw, T2_HW_I2C1_INT_STATUS);
    p->divisor = switch_t2_hw_get(hw, T2_HW_I2C1_CLK_DIVISOR);
    p->config_load = switch_t2_hw_get(hw, T2_HW_I2C1_CONFIG_LOAD);
    p->timing_0 = switch_t2_hw_get(hw, T2_HW_I2C1_TIMING_0);
    p->timing_1 = switch_t2_hw_get(hw, T2_HW_I2C1_TIMING_1);
    if (hw->stop)
        return 0;
    if (p->status & STATUS_BUSY)
        switch_t2_hw_stop(hw, SWITCH_T2_BUS_BUSY);
    else if (p->int_status)
        switch_t2_hw_stop(hw, SWITCH_T2_INT_PENDING);
    else if (p->divisor != SWITCH_T2_DIVISOR_BEFORE)
        switch_t2_hw_stop(hw, SWITCH_T2_DIVISOR_UNEXPECTED);
    else if (p->config_load & LOAD_PENDING)
        switch_t2_hw_stop(hw, SWITCH_T2_LOAD_BEFORE_PENDING);
    else if (p->config_load != SWITCH_T2_LOAD_REST)
        switch_t2_hw_stop(hw, SWITCH_T2_LOAD_BEFORE_UNEXPECTED);
    else {
        const int t0 = timing_stop(p->timing_0, hw->io->timing_0_pin);
        const int t1 = t0 ? t0 : timing_stop(p->timing_1, hw->io->timing_1_pin);
        if (t1)
            switch_t2_hw_stop(hw, t1);
    }
    return !hw->stop;
}

/* Three samples, spaced, all idle and all equal to the first read. */
static int bus_idle(struct switch_t2_hw *hw, const struct switch_t2_probe *p)
{
    for (unsigned i = 0; i < SWITCH_T2_IDLE_SAMPLES; ++i) {
        if (i && !switch_t2_hw_wait_us(hw, SWITCH_T2_IDLE_SPACING_US))
            return 0;
        const uint32_t status = switch_t2_hw_get(hw, T2_HW_I2C1_STATUS);
        const uint32_t pending = switch_t2_hw_get(hw, T2_HW_I2C1_INT_STATUS);
        if (hw->stop)
            return 0;
        if ((status & STATUS_BUSY) || pending || status != p->status) {
            switch_t2_hw_stop(hw, SWITCH_T2_IDLE_FAILED);
            return 0;
        }
    }
    return 1;
}

/* The one controller change, as T1 v3 made it. Nothing is written back afterwards. */
static int apply_divisor(struct switch_t2_hw *hw, struct switch_t2_probe *p)
{
    switch_t2_hw_put(hw, T2_HW_I2C1_CLK_DIVISOR, SWITCH_T2_DIVISOR_AFTER);
    if (hw->stop || !switch_t2_hw_load(hw, SWITCH_T2_LOAD_APPLY, SWITCH_T2_CONFIG_LOAD_TIMEOUT))
        return 0;
    p->load_after = switch_t2_hw_get(hw, T2_HW_I2C1_CONFIG_LOAD);
    p->divisor_after = switch_t2_hw_get(hw, T2_HW_I2C1_CLK_DIVISOR);
    p->status_load = switch_t2_hw_get(hw, T2_HW_I2C1_STATUS);
    p->int_status_load = switch_t2_hw_get(hw, T2_HW_I2C1_INT_STATUS);
    if (hw->stop)
        return 0;
    if (p->divisor_after != SWITCH_T2_DIVISOR_AFTER)
        switch_t2_hw_stop(hw, SWITCH_T2_DIVISOR_READBACK);
    else if (p->load_after != 0 && p->load_after != SWITCH_T2_LOAD_REST)
        switch_t2_hw_stop(hw, SWITCH_T2_LOAD_AFTER_UNEXPECTED);
    else if (p->status_load & STATUS_BUSY)
        switch_t2_hw_stop(hw, SWITCH_T2_NOT_IDLE);
    else if (p->int_status_load != p->int_status)
        switch_t2_hw_stop(hw, SWITCH_T2_INT_CHANGED);
    return !hw->stop;
}

int switch_t2_prelude(struct switch_t2_hw *hw, struct switch_t2_probe *probe)
{
    /* A struct assignment would call memset, which this image does not link. */
    for (volatile uint8_t *b = (volatile uint8_t *)probe; b < (volatile uint8_t *)(probe + 1); ++b)
        *b = 0;
    if (!counter_advances(hw->io)) {
        switch_t2_hw_stop(hw, SWITCH_T2_COUNTER_STALLED);
        return 0;
    }
    return read_car(hw, probe) && read_controller(hw, probe) && bus_idle(hw, probe) && apply_divisor(hw, probe);
}
