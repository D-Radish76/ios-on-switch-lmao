#include "t3.h"

static unsigned level(uint32_t word)
{
    return word >> SWITCH_T3_TACH_BIT & 1u;
}

/* The pad is read as fast as the loop goes. hekate's fan_get_speed does the same for 2 s; interrupts would need GIC and GPIO
 * interrupt writes no run has tried. The rail limit is checked on every pass, so a window never outlives it. */
void switch_t3_count_window(const struct switch_t3_io *io, struct switch_t3_result *result, unsigned id, unsigned milliseconds)
{
    struct switch_t3_window *w = &result->window[id];
    w->ran = 1;
    io->window_begin(io->context, milliseconds);
    w->first_level = w->last_level = level(io->read(io->context, T3_GPIO_S_IN));
    w->samples = 1;
    w->high = w->first_level;
    for (uint32_t pass = 0; pass < SWITCH_T3_SAMPLE_LIMIT; ++pass) {
        if (SWITCH_T3_RAIL_LIVE(result->rail) && io->rail_expired(io->context)) {
            w->cut = T3_CUT_RAIL;
            if (!result->stop)
                result->stop = SWITCH_T3_RAIL_TIME;
            return;
        }
        if (io->window_over(io->context))
            return;
        const unsigned now = level(io->read(io->context, T3_GPIO_S_IN));
        ++w->samples;
        w->high += now;
        if (now != w->last_level) {
            ++w->transitions;
            w->last_level = now;
        }
    }
    w->cut = T3_CUT_LIMIT;
    if (!result->stop)
        result->stop = SWITCH_T3_WINDOW;
}

static uint32_t larger(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

unsigned switch_t3_verdict(const struct switch_t3_window window[T3_WINDOW_COUNT])
{
    const struct switch_t3_window *idle = &window[T3_WINDOW_IDLE];
    const struct switch_t3_window *rail = &window[T3_WINDOW_RAIL];
    const struct switch_t3_window *spin = &window[T3_WINDOW_SPIN];
    if (!idle->ran || !rail->ran || !spin->ran)
        return T3_VERDICT_NONE;
    if (idle->samples < SWITCH_T3_MIN_SAMPLES || rail->samples < SWITCH_T3_MIN_SAMPLES ||
        spin->samples < SWITCH_T3_MIN_SAMPLES)
        return T3_VERDICT_FEW_SAMPLES;
    if (spin->transitions >= SWITCH_T3_MIN_TRANSITIONS && spin->transitions > larger(idle->transitions, rail->transitions))
        return T3_VERDICT_PASS;
    return T3_VERDICT_NO_EDGES;
}

static unsigned bit(uint32_t word, unsigned position)
{
    return word >> position & 1u;
}

/* What hekate's fan and rail enable leave, as T3-R measured it at entry; PWM_CSR_1 is zero there, not hekate's off word. */
unsigned switch_t3_state_check(const uint32_t v[T3_REG_COUNT])
{
    if (!(v[T3_CAR_CLK_OUT_ENB_L] & SWITCH_T3_CAR_PWM_BIT) || (v[T3_CAR_RST_DEVICES_L] & SWITCH_T3_CAR_PWM_BIT))
        return T3_STATE_GATE;
    if (v[T3_PWM_CSR_1] != 0)
        return T3_STATE_PWM;
    if (v[T3_PINMUX_LCD_GPIO2] != SWITCH_T3_PARK_PAD_WORD)
        return T3_STATE_LCD_PAD;
    if (v[T3_PINMUX_CAM1_PWDN] != 0x59u)
        return T3_STATE_TACH_PAD;
    if (v[T3_PINMUX_SATA_LED_ACTIVE] != 0x01u)
        return T3_STATE_RAIL_PIN;
    if (v[T3_PINMUX_USB_VBUS_EN0] != 0x101u)
        return T3_STATE_USB_PIN;
    if (!bit(v[T3_GPIO_A_CNF], 5) || !bit(v[T3_GPIO_A_OE], 5) || v[T3_GPIO_A_OUT] != SWITCH_T3_RAIL_OFF_WORD)
        return T3_STATE_A5;
    if (!bit(v[T3_GPIO_CC_CNF], 4) || !bit(v[T3_GPIO_CC_OE], 4) || bit(v[T3_GPIO_CC_OUT], 4))
        return T3_STATE_CC4;
    if (!bit(v[T3_GPIO_S_CNF], SWITCH_T3_TACH_BIT) || bit(v[T3_GPIO_S_OE], SWITCH_T3_TACH_BIT))
        return T3_STATE_S7;
    if (bit(v[T3_GPIO_V_CNF], 4))
        return T3_STATE_V4;
    if (v[T3_PMC_NO_IOPOWER] != 0 || bit(v[T3_PMC_PWR_DET_VAL], SWITCH_T3_PWR_DET_GPIO_BIT))
        return T3_STATE_PMC;
    return T3_STATE_OK;
}

uint64_t switch_t3_ms_ticks(uint64_t cntfrq, unsigned milliseconds)
{
    return cntfrq == SWITCH_T3_CNTFRQ ? cntfrq / 1000u * milliseconds : 0;
}
