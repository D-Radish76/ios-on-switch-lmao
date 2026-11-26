#include "t4r.h"

static int shown(const struct switch_t4r_io *io, struct switch_t4r_result *result, unsigned step)
{
    if (io->show && !io->show(io->context, step, result)) {
        result->stop = SWITCH_T4R_NOT_SHOWN;
        return 0;
    }
    return 1;
}

static uint32_t get(const struct switch_t4r_io *io, struct switch_t4r_result *result, unsigned reg)
{
    if (!switch_t4r_may_read(reg, T4R_WIDTH)) {
        result->stop = SWITCH_T4R_DENIED;
        return 0;
    }
    ++result->reads;
    return io->read(io->context, reg);
}

/* Shows BEGIN, reads, shows RESULT. Returns 0 when the run has to end: a refused read or a line that did not settle. */
static int step_read(const struct switch_t4r_io *io, struct switch_t4r_result *result, unsigned reg, uint32_t *value,
                     unsigned begin)
{
    if (!shown(io, result, begin))
        return 0;
    *value = get(io, result, reg);
    if (result->stop)
        return 0;
    return shown(io, result, begin + 1);
}

static void finish(const struct switch_t4r_io *io, struct switch_t4r_result *result)
{
    const int stopped = result->stop;
    int ok = 1;
    if (io->show)
        ok = io->show(io->context, T4R_STEP_COMPLETE, result);
    if (!ok && !stopped)
        result->stop = SWITCH_T4R_NOT_SHOWN;
}

unsigned switch_t4r_classify(uint32_t ptv, uint32_t config, uint32_t status, unsigned *why)
{
    *why = T4R_WHY_NONE;
    if (ptv == SWITCH_T4R_ALL_ONES || config == SWITCH_T4R_ALL_ONES || status == SWITCH_T4R_ALL_ONES) {
        *why = T4R_WHY_ALL_ONES;
        return T4R_STATE_UNKNOWN;
    }
    if (status & ~(SWITCH_T4R_STS_ENABLED | SWITCH_T4R_STS_INT)) {
        *why = T4R_WHY_STATUS_BITS;
        return T4R_STATE_UNKNOWN;
    }
    if ((ptv & SWITCH_T4R_PTV_ENABLE) || (status & SWITCH_T4R_STS_INT))
        return T4R_STATE_BUSY;
    if (ptv) {
        *why = T4R_WHY_TIMER_VALUE;
        return T4R_STATE_UNKNOWN;
    }
    if (status & SWITCH_T4R_STS_ENABLED) {
        if (SWITCH_T4R_CFG_SOURCE(config) != SWITCH_T4R_WDT0_TIMER) {
            *why = T4R_WHY_OTHER_SOURCE;
            return T4R_STATE_UNKNOWN;
        }
        return T4R_STATE_DORMANT;
    }
    return config ? T4R_STATE_CONFIGURED_STOPPED : T4R_STATE_IDLE;
}

int switch_t4r_advanced(uint32_t first, uint32_t second)
{
    const uint32_t delta = second - first;
    return delta != 0 && delta < 0x80000000u;
}

void switch_t4r_run(const struct switch_t4r_io *io, struct switch_t4r_result *result)
{
    /* A struct assignment would call memset, which the loader image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    if (shown(io, result, T4R_STEP_START) &&
        step_read(io, result, T4R_TMRUS, &result->tmrus_first, T4R_STEP_TMRUS1_BEGIN) &&
        step_read(io, result, T4R_TMR5_PTV, &result->ptv, T4R_STEP_PTV_BEGIN) &&
        step_read(io, result, T4R_WDT0_CONFIG, &result->config, T4R_STEP_CONFIG_BEGIN) &&
        step_read(io, result, T4R_WDT0_STATUS, &result->status, T4R_STEP_STATUS_BEGIN)) {
        result->state = switch_t4r_classify(result->ptv, result->config, result->status, &result->why);
        if (shown(io, result, T4R_STEP_STATE) &&
            step_read(io, result, T4R_TMRUS, &result->tmrus_second, T4R_STEP_TMRUS2_BEGIN))
            result->tmrus_advanced = switch_t4r_advanced(result->tmrus_first, result->tmrus_second);
    }
    finish(io, result);
}

uint64_t switch_t4r_settle_ticks(uint64_t cntfrq)
{
    return cntfrq == SWITCH_T4R_CNTFRQ ? cntfrq / 1000u * SWITCH_T4R_SETTLE_MS : 0;
}
