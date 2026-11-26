#include "t4r.h"

static const uint32_t table[T4R_REG_COUNT] = {
    [T4R_TMRUS] = 0x60005010u,
    [T4R_TMR5_PTV] = 0x60005060u,
    [T4R_WDT0_CONFIG] = 0x60005100u,
    [T4R_WDT0_STATUS] = 0x60005104u,
};

uint32_t switch_t4r_address(unsigned reg)
{
    return reg < T4R_REG_COUNT ? table[reg] : 0;
}

int switch_t4r_may_read(unsigned reg, unsigned width)
{
    return reg < T4R_REG_COUNT && width == T4R_WIDTH;
}
