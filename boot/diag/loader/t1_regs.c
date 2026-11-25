#include "t1.h"

#define CAR  0x60006000u
#define MUX  0x70003000u
#define I2C1 0x7000c000u

enum { R = 1, W = 2 };

static const struct {
    uint32_t address;
    uint8_t access;
} table[T1_REG_COUNT] = {
    [T1_CAR_RST_DEVICES_L] = {CAR + 0x004, R},
    [T1_CAR_CLK_OUT_ENB_L] = {CAR + 0x010, R},
    [T1_CAR_CLK_SOURCE_PWM] = {CAR + 0x110, R},
    [T1_CAR_CLK_SOURCE_I2C1] = {CAR + 0x124, R},
    [T1_PINMUX_I2C1_SCL] = {MUX + 0x0bc, R},
    [T1_PINMUX_I2C1_SDA] = {MUX + 0x0c0, R},
    [T1_I2C1_CNFG] = {I2C1 + 0x00, R | W},
    [T1_I2C1_CMD_ADDR0] = {I2C1 + 0x04, W},
    [T1_I2C1_CMD_DATA1] = {I2C1 + 0x0c, R | W},
    [T1_I2C1_STATUS] = {I2C1 + 0x1c, R},
    [T1_I2C1_INT_STATUS] = {I2C1 + 0x68, R},
    [T1_I2C1_CLK_DIVISOR] = {I2C1 + 0x6c, R},
    [T1_I2C1_CONFIG_LOAD] = {I2C1 + 0x8c, R | W},
};

const uint8_t switch_t1_tmp451_register[SWITCH_T1_REGISTERS] = {0xfe, 0x09, 0x0a, 0x00, 0x01};

uint32_t switch_t1_address(unsigned reg)
{
    return reg < T1_REG_COUNT ? table[reg].address : 0;
}

int switch_t1_may_read(unsigned reg)
{
    return reg < T1_REG_COUNT && (table[reg].access & R);
}

int switch_t1_may_write(unsigned reg, uint32_t value)
{
    if (reg >= T1_REG_COUNT || !(table[reg].access & W))
        return 0;
    switch (reg) {
    case T1_I2C1_CMD_ADDR0: /* 0x4c << 1, then the write or the read bit */
        return value == 0x98u || value == 0x99u;
    case T1_I2C1_CMD_DATA1: { /* the register pointer, one byte */
        for (unsigned i = 0; i < SWITCH_T1_REGISTERS; ++i)
            if (value == switch_t1_tmp451_register[i])
                return 1;
        return 0;
    }
    case T1_I2C1_CNFG: /* one byte, 4 clock debounce, new master FSM; write or read; without and with the go bit */
        return value == 0x2800u || value == 0x2840u || value == 0x2a00u || value == 0x2a40u;
    case T1_I2C1_CONFIG_LOAD: /* hekate's value: bit 5, timeout load, master load */
        return value == 0x25u;
    default:
        return 0;
    }
}
