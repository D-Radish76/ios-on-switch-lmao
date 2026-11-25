#include "t1t.h"

#define CLK_L_I2C1 (1u << 12)
#define CLK_H_I2C5 (1u << 15)

static uint32_t get(const struct switch_t1t_io *io, struct switch_t1t_result *result, unsigned reg)
{
    if (!switch_t1t_may_read(reg)) {
        result->stop = SWITCH_T1T_DENIED;
        return 0;
    }
    ++result->reads;
    return io->read(io->context, reg);
}

static void read_block(const struct switch_t1t_io *io, struct switch_t1t_result *result, unsigned block, int on, int reset)
{
    struct switch_t1t_block *out = &result->block[block];
    if (!on) {
        out->state = SWITCH_T1T_BLOCK_CLOCK_OFF;
        return;
    }
    if (reset) {
        out->state = SWITCH_T1T_BLOCK_IN_RESET;
        return;
    }
    for (unsigned word = 0; word < T1T_BLOCK_WORDS; ++word)
        out->word[word] = get(io, result, SWITCH_T1T_I2C_REG(block, word));
}

void switch_t1t_run(const struct switch_t1t_io *io, struct switch_t1t_result *result)
{
    /* A struct assignment would call memset, which this image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    result->rst_devices_l = get(io, result, T1T_CAR_RST_DEVICES_L);
    result->rst_devices_h = get(io, result, T1T_CAR_RST_DEVICES_H);
    result->clk_out_enb_l = get(io, result, T1T_CAR_CLK_OUT_ENB_L);
    result->clk_out_enb_h = get(io, result, T1T_CAR_CLK_OUT_ENB_H);
    for (unsigned bus = 0; bus < SWITCH_T1T_BUSES; ++bus)
        result->source[bus] = get(io, result, T1T_CAR_SOURCE_I2C1 + bus);
    result->pllp_base = get(io, result, T1T_CAR_PLLP_BASE);
    if (result->stop)
        return;
    read_block(io, result, 0, result->clk_out_enb_l & CLK_L_I2C1, result->rst_devices_l & CLK_L_I2C1);
    read_block(io, result, 1, result->clk_out_enb_h & CLK_H_I2C5, result->rst_devices_h & CLK_H_I2C5);
}
