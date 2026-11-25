#include "t1r.h"

#define CLK_L_I2C1 (1u << 12)

static uint32_t get(const struct switch_t1r_io *io, struct switch_t1r_result *result, unsigned reg)
{
    if (!switch_t1r_may_read(reg)) {
        result->stop = SWITCH_T1R_DENIED;
        return 0;
    }
    return io->read(io->context, reg);
}

void switch_t1r_run(const struct switch_t1r_io *io, struct switch_t1r_result *result)
{
    /* A struct assignment would call memset, which this image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    result->rst_devices_l = get(io, result, T1R_CAR_RST_DEVICES_L);
    result->clk_out_enb_l = get(io, result, T1R_CAR_CLK_OUT_ENB_L);
    for (unsigned bus = 0; bus < SWITCH_T1R_BUSES; ++bus)
        result->source[bus] = get(io, result, T1R_CAR_SOURCE_I2C1 + bus);
    result->pllp_base = get(io, result, T1R_CAR_PLLP_BASE);
    result->pllp_outa = get(io, result, T1R_CAR_PLLP_OUTA);
    result->pllp_outb = get(io, result, T1R_CAR_PLLP_OUTB);
    if (result->stop)
        return;
    if (!(result->clk_out_enb_l & CLK_L_I2C1)) {
        result->i2c1 = SWITCH_T1R_I2C1_CLOCK_OFF;
        return;
    }
    if (result->rst_devices_l & CLK_L_I2C1) {
        result->i2c1 = SWITCH_T1R_I2C1_IN_RESET;
        return;
    }
    result->cnfg = get(io, result, T1R_I2C1_CNFG);
    result->status = get(io, result, T1R_I2C1_STATUS);
    result->int_status = get(io, result, T1R_I2C1_INT_STATUS);
    result->divisor = get(io, result, T1R_I2C1_CLK_DIVISOR);
    result->config_load = get(io, result, T1R_I2C1_CONFIG_LOAD);
}
