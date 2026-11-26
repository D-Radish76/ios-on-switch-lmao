#include "s2.h"

static uint32_t get(const struct switch_s2_io *io, struct switch_s2_result *result, unsigned reg)
{
    if (!switch_s2_may_read(reg)) {
        result->stop = SWITCH_S2_DENIED;
        return 0;
    }
    ++result->reads;
    return io->read(io->context, reg);
}

void switch_s2_run(const struct switch_s2_io *io, struct switch_s2_result *result)
{
    /* A struct assignment would call memset, which the loader image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    result->rst_devices_l = get(io, result, S2_CAR_RST_DEVICES_L);
    result->clk_out_enb_l = get(io, result, S2_CAR_CLK_OUT_ENB_L);
    if (result->stop)
        return;
    if (!(result->clk_out_enb_l & SWITCH_S2_SDMMC1_CAR_BIT)) {
        result->sd_state = SWITCH_S2_SD_CLOCK_OFF;
        return;
    }
    if (result->rst_devices_l & SWITCH_S2_SDMMC1_CAR_BIT) {
        result->sd_state = SWITCH_S2_SD_IN_RESET;
        return;
    }
    result->sd_present_state = get(io, result, S2_SDMMC1_PRESENT_STATE);
    result->sd_word_2c = get(io, result, S2_SDMMC1_WORD_2C);
}
