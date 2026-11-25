#include "t1r.h"

#define CAR  0x60006000u
#define I2C1 0x7000c000u

static const uint32_t table[T1R_REG_COUNT] = {
    [T1R_CAR_RST_DEVICES_L] = CAR + 0x004,
    [T1R_CAR_CLK_OUT_ENB_L] = CAR + 0x010,
    [T1R_CAR_SOURCE_I2C1] = CAR + 0x124,
    [T1R_CAR_SOURCE_I2C2] = CAR + 0x198,
    [T1R_CAR_SOURCE_I2C3] = CAR + 0x1b8,
    [T1R_CAR_SOURCE_I2C4] = CAR + 0x3c4,
    [T1R_CAR_SOURCE_I2C5] = CAR + 0x128,
    [T1R_CAR_SOURCE_I2C6] = CAR + 0x65c,
    [T1R_CAR_PLLP_BASE] = CAR + 0x0a0,
    [T1R_CAR_PLLP_OUTA] = CAR + 0x0a4,
    [T1R_CAR_PLLP_OUTB] = CAR + 0x0a8,
    [T1R_I2C1_CNFG] = I2C1 + 0x00,
    [T1R_I2C1_STATUS] = I2C1 + 0x1c,
    [T1R_I2C1_INT_STATUS] = I2C1 + 0x68,
    [T1R_I2C1_CLK_DIVISOR] = I2C1 + 0x6c,
    [T1R_I2C1_CONFIG_LOAD] = I2C1 + 0x8c,
};

uint32_t switch_t1r_address(unsigned reg)
{
    return reg < T1R_REG_COUNT ? table[reg] : 0;
}

int switch_t1r_may_read(unsigned reg)
{
    return reg < T1R_REG_COUNT;
}

/* hekate's _clock_i2c table: clock source 6 (clk_m) with CLK_I2C_SRC_DIV(4) for I2C1, I2C4 and I2C6 (divider field 3) and
 * CLK_I2C_SRC_DIV(1) for the others (divider field 0). */
uint32_t switch_t1r_hekate_source(unsigned bus)
{
    static const uint8_t divider[SWITCH_T1R_BUSES] = {3, 0, 0, 3, 0, 3};
    return bus < SWITCH_T1R_BUSES ? (6u << 29) | divider[bus] : 0;
}
