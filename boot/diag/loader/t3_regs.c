#include "t3.h"

static const uint32_t table[T3_REG_COUNT] = {
    [T3_CAR_RST_DEVICES_L] = 0x60006004u,
    [T3_CAR_CLK_OUT_ENB_L] = 0x60006010u,
    [T3_PWM_CSR_1] = 0x7000a010u,
    [T3_PINMUX_LCD_GPIO2] = 0x7000320cu,
    [T3_PINMUX_CAM1_PWDN] = 0x700031ecu,
    [T3_PINMUX_SATA_LED_ACTIVE] = 0x7000304cu,
    [T3_PINMUX_USB_VBUS_EN0] = 0x700031a8u,
    [T3_GPIO_A_CNF] = 0x6000d000u,
    [T3_GPIO_A_OE] = 0x6000d010u,
    [T3_GPIO_A_OUT] = 0x6000d020u,
    [T3_GPIO_CC_CNF] = 0x6000d700u,
    [T3_GPIO_CC_OE] = 0x6000d710u,
    [T3_GPIO_CC_OUT] = 0x6000d720u,
    [T3_GPIO_S_CNF] = 0x6000d408u,
    [T3_GPIO_S_OE] = 0x6000d418u,
    [T3_GPIO_S_IN] = 0x6000d438u,
    [T3_GPIO_V_CNF] = 0x6000d504u,
    [T3_GPIO_V_OE] = 0x6000d514u,
    [T3_PMC_NO_IOPOWER] = 0x7000e444u,
    [T3_PMC_PWR_DET_VAL] = 0x7000e4e4u,
};

static const struct {
    uint8_t reg;
    uint32_t value;
} writes[T3_WRITE_COUNT] = {
    [T3_W_RAIL_ON] = {T3_GPIO_A_OUT, SWITCH_T3_RAIL_ON_WORD},
    [T3_W_PWM_DUTY] = {T3_PWM_CSR_1, SWITCH_T3_PWM_DUTY51},
    [T3_W_ROUTE_PWM] = {T3_PINMUX_LCD_GPIO2, SWITCH_T3_ROUTE_PWM_WORD},
    [T3_O_PWM_OFF] = {T3_PWM_CSR_1, SWITCH_T3_PWM_OFF_WORD},
    [T3_O_RAIL_OFF] = {T3_GPIO_A_OUT, SWITCH_T3_RAIL_OFF_WORD},
    [T3_O_PARK_PAD] = {T3_PINMUX_LCD_GPIO2, SWITCH_T3_PARK_PAD_WORD},
};

uint32_t switch_t3_address(unsigned reg)
{
    return reg < T3_REG_COUNT ? table[reg] : 0;
}

int switch_t3_may_read(unsigned reg, unsigned width)
{
    return reg < T3_REG_COUNT && width == T3_WIDTH;
}

int switch_t3_may_write(unsigned reg, uint32_t value)
{
    for (unsigned w = 0; w < T3_WRITE_COUNT; ++w)
        if (writes[w].reg == reg && writes[w].value == value)
            return 1;
    return 0;
}

unsigned switch_t3_write_reg(unsigned write)
{
    return write < T3_WRITE_COUNT ? writes[write].reg : T3_REG_COUNT;
}

uint32_t switch_t3_write_value(unsigned write)
{
    return write < T3_WRITE_COUNT ? writes[write].value : 0;
}
