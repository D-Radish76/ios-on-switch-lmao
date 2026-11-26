#include "t2_regs.h"

#define CAR  0x60006000u
#define I2C1 0x7000c000u

enum { R = 1, W = 2 };

static const struct {
    uint32_t address;
    uint8_t access;
} table[T2_HW_REG_COUNT] = {
    [T2_HW_CAR_RST_DEVICES_L] = {CAR + 0x004, R},
    [T2_HW_CAR_CLK_OUT_ENB_L] = {CAR + 0x010, R},
    [T2_HW_CAR_CLK_SOURCE_I2C1] = {CAR + 0x124, R},
    [T2_HW_CAR_PLLP_BASE] = {CAR + 0x0a0, R},
    [T2_HW_I2C1_CNFG] = {I2C1 + 0x00, R | W},
    [T2_HW_I2C1_CMD_ADDR0] = {I2C1 + 0x04, W},
    [T2_HW_I2C1_CMD_DATA1] = {I2C1 + 0x0c, R | W},
    [T2_HW_I2C1_STATUS] = {I2C1 + 0x1c, R},
    [T2_HW_I2C1_INT_STATUS] = {I2C1 + 0x68, R},
    [T2_HW_I2C1_CLK_DIVISOR] = {I2C1 + 0x6c, R | W},
    [T2_HW_I2C1_CONFIG_LOAD] = {I2C1 + 0x8c, R | W},
    [T2_HW_I2C1_TIMING_0] = {I2C1 + 0x94, R},
    [T2_HW_I2C1_TIMING_1] = {I2C1 + 0x98, R},
};

uint32_t switch_t2_address(unsigned reg)
{
    return reg < T2_HW_REG_COUNT ? table[reg].address : 0;
}

int switch_t2_may_read(unsigned reg)
{
    return reg < T2_HW_REG_COUNT && (table[reg].access & R);
}

int switch_t2_may_write(unsigned reg, uint32_t value)
{
    if (reg >= T2_HW_REG_COUNT || !(table[reg].access & W))
        return 0;
    switch (reg) {
    case T2_HW_I2C1_CMD_ADDR0: /* 0x4c << 1, then the write or the read bit */
        return value == 0x98u || value == 0x99u;
    case T2_HW_I2C1_CMD_DATA1: /* the register pointers of ERISTA_T2_DESIGN.md (0F with value 00 is the one-shot, word 0000000F) */
        switch (value) {
        case 0xfe: case 0x03: case 0x11: case 0x12: case 0x02: case 0x01: case 0x10: case 0x00: case 0x15: case 0x0f:
            return 1;
        default:
            return 0;
        }
    case T2_HW_I2C1_CNFG: /* one byte, or two for the one-shot; write or read; without and with the go bit */
        return value == 0x2800u || value == 0x2840u || value == 0x2a00u || value == 0x2a40u || value == 0x2802u ||
               value == 0x2a02u;
    case T2_HW_I2C1_CLK_DIVISOR: /* the one change, as T1 v3 made it */
        return value == SWITCH_T2_DIVISOR_AFTER;
    case T2_HW_I2C1_CONFIG_LOAD: /* 1 applies the divisor; 0x25 loads a transfer */
        return value == SWITCH_T2_LOAD_APPLY || value == SWITCH_T2_LOAD_TRANSFER;
    default:
        return 0;
    }
}
