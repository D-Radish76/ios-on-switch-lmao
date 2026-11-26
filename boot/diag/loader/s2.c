#include "s2.h"

static int shown(const struct switch_s2_io *io, struct switch_s2_result *result, unsigned step)
{
    if (io->show && !io->show(io->context, step, result)) {
        result->stop = SWITCH_S2_NOT_SHOWN;
        return 0;
    }
    return 1;
}

static uint32_t get(const struct switch_s2_io *io, struct switch_s2_result *result, unsigned reg)
{
    if (!switch_s2_may_read(reg)) {
        result->stop = SWITCH_S2_DENIED;
        return 0;
    }
    ++result->reads;
    return io->read(io->context, reg);
}

/* Shows BEGIN, reads, shows RESULT. Returns 0 when the run has to end: a refused read or a line that did not settle. */
static int step_read(const struct switch_s2_io *io, struct switch_s2_result *result, unsigned reg, uint32_t *value,
                     unsigned begin)
{
    if (!shown(io, result, begin))
        return 0;
    *value = get(io, result, reg);
    if (result->stop)
        return 0;
    return shown(io, result, begin + 1);
}

static void finish(const struct switch_s2_io *io, struct switch_s2_result *result)
{
    const int stopped = result->stop;
    int ok = 1;
    if (io->show)
        ok = io->show(io->context, S2_STEP_COMPLETE, result);
    if (!ok && !stopped)
        result->stop = SWITCH_S2_NOT_SHOWN;
}

void switch_s2_run(const struct switch_s2_io *io, struct switch_s2_result *result)
{
    /* A struct assignment would call memset, which the loader image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    if (shown(io, result, S2_STEP_START) && step_read(io, result, S2_CAR_RST_DEVICES_L, &result->rst_devices_l, S2_STEP_CAR_RST_BEGIN) &&
        step_read(io, result, S2_CAR_CLK_OUT_ENB_L, &result->clk_out_enb_l, S2_STEP_CAR_CLK_BEGIN)) {
        if (!(result->clk_out_enb_l & SWITCH_S2_SDMMC1_CAR_BIT))
            result->sd_state = SWITCH_S2_SD_CLOCK_OFF;
        else if (result->rst_devices_l & SWITCH_S2_SDMMC1_CAR_BIT)
            result->sd_state = SWITCH_S2_SD_IN_RESET;
        if (shown(io, result, S2_STEP_GATE) && result->sd_state == SWITCH_S2_SD_READ &&
            step_read(io, result, S2_SDMMC1_PRESENT_STATE, &result->sd_present_state, S2_STEP_PRESENT_BEGIN))
            step_read(io, result, S2_SDMMC1_WORD_2C, &result->sd_word_2c, S2_STEP_CLOCK_WORD_BEGIN);
    }
    finish(io, result);
}

uint64_t switch_s2_settle_ticks(uint64_t cntfrq)
{
    return cntfrq == SWITCH_S2_CNTFRQ ? cntfrq / 1000u * SWITCH_S2_SETTLE_MS : 0;
}
