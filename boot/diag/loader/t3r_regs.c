#include "t3r.h"

static const uint32_t table[T3R_REG_COUNT] = {
    [T3R_TMRUS] = 0x60005010u,
    [T3R_CAR_RST_DEVICES_L] = 0x60006004u,
    [T3R_CAR_CLK_OUT_ENB_L] = 0x60006010u,
    [T3R_PWM_CSR_1] = 0x7000a010u,
    [T3R_PINMUX_LCD_GPIO2] = 0x7000320cu,
    [T3R_PINMUX_CAM1_PWDN] = 0x700031ecu,
    [T3R_PINMUX_SATA_LED_ACTIVE] = 0x7000304cu,
    [T3R_PINMUX_USB_VBUS_EN0] = 0x700031a8u,
    [T3R_GPIO_A_CNF] = 0x6000d000u,
    [T3R_GPIO_A_OE] = 0x6000d010u,
    [T3R_GPIO_A_OUT] = 0x6000d020u,
    [T3R_GPIO_CC_CNF] = 0x6000d700u,
    [T3R_GPIO_CC_OE] = 0x6000d710u,
    [T3R_GPIO_CC_OUT] = 0x6000d720u,
    [T3R_GPIO_S_CNF] = 0x6000d408u,
    [T3R_GPIO_S_OE] = 0x6000d418u,
    [T3R_GPIO_S_IN] = 0x6000d438u,
    [T3R_GPIO_V_CNF] = 0x6000d504u,
    [T3R_GPIO_V_OE] = 0x6000d514u,
    [T3R_PMC_NO_IOPOWER] = 0x7000e444u,
    [T3R_PMC_PWR_DET_VAL] = 0x7000e4e4u,
};

/* The PMC reads come last: whether a non-secure EL1 read of them answers is not known. */
static const uint8_t sequence[T3R_READ_COUNT] = {
    T3R_TMRUS,
    T3R_CAR_RST_DEVICES_L,
    T3R_CAR_CLK_OUT_ENB_L,
    T3R_PWM_CSR_1,
    T3R_PINMUX_LCD_GPIO2,
    T3R_PINMUX_CAM1_PWDN,
    T3R_PINMUX_SATA_LED_ACTIVE,
    T3R_PINMUX_USB_VBUS_EN0,
    T3R_GPIO_A_CNF,
    T3R_GPIO_A_OE,
    T3R_GPIO_A_OUT,
    T3R_GPIO_CC_CNF,
    T3R_GPIO_CC_OE,
    T3R_GPIO_CC_OUT,
    T3R_GPIO_S_CNF,
    T3R_GPIO_S_OE,
    T3R_GPIO_S_IN,
    T3R_GPIO_V_CNF,
    T3R_GPIO_V_OE,
    T3R_PMC_NO_IOPOWER,
    T3R_PMC_PWR_DET_VAL,
    T3R_TMRUS,
};

unsigned switch_t3r_sequence(unsigned index)
{
    return index < T3R_READ_COUNT ? sequence[index] : T3R_REG_COUNT;
}

uint32_t switch_t3r_address(unsigned reg)
{
    return reg < T3R_REG_COUNT ? table[reg] : 0;
}

int switch_t3r_may_read(unsigned reg, unsigned width)
{
    return reg < T3R_REG_COUNT && width == T3R_WIDTH;
}
