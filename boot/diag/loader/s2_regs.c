#include "s2.h"

static const uint32_t table[S2_REG_COUNT] = {
    [S2_CAR_RST_DEVICES_L] = 0x60006004u,
    [S2_CAR_CLK_OUT_ENB_L] = 0x60006010u,
    [S2_SDMMC1_PRESENT_STATE] = 0x700b0024u,
    [S2_SDMMC1_WORD_2C] = 0x700b002cu,
};

uint32_t switch_s2_address(unsigned reg)
{
    return reg < S2_REG_COUNT ? table[reg] : 0;
}

int switch_s2_may_read(unsigned reg)
{
    return reg < S2_REG_COUNT;
}
