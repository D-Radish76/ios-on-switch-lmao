#include "s2.h"

static const struct {
    uint32_t address;
    unsigned width;
} table[S2_REG_COUNT] = {
    [S2_CAR_RST_DEVICES_L] = {0x60006004u, S2_WIDTH_32},
    [S2_CAR_CLK_OUT_ENB_L] = {0x60006010u, S2_WIDTH_32},
    [S2_SDMMC1_PRESENT_STATE] = {0x700b0024u, S2_WIDTH_32},
    [S2_SDMMC1_CLOCK_CONTROL] = {0x700b002cu, S2_WIDTH_16},
};

uint32_t switch_s2_address(unsigned reg)
{
    return reg < S2_REG_COUNT ? table[reg].address : 0;
}

unsigned switch_s2_width(unsigned reg)
{
    return reg < S2_REG_COUNT ? table[reg].width : 0;
}

int switch_s2_may_read(unsigned reg, unsigned width)
{
    return reg < S2_REG_COUNT && width == table[reg].width;
}
