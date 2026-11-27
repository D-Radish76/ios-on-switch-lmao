#include "t3r.h"

static int shown(const struct switch_t3r_io *io, struct switch_t3r_result *result, unsigned step)
{
    if (io->show && !io->show(io->context, step, result)) {
        result->stop = SWITCH_T3R_NOT_SHOWN;
        return 0;
    }
    return 1;
}

static uint32_t get(const struct switch_t3r_io *io, struct switch_t3r_result *result, unsigned reg)
{
    if (!switch_t3r_may_read(reg, T3R_WIDTH)) {
        result->stop = SWITCH_T3R_DENIED;
        return 0;
    }
    ++result->reads;
    return io->read(io->context, reg);
}

/* Shows BEGIN, reads, derives what the RESULT line shows, shows RESULT. Returns 0 when the run has to end: a refused read or a line that did not settle. */
static void derive(struct switch_t3r_result *result, unsigned reg, int second_tmrus);

static int step_read(const struct switch_t3r_io *io, struct switch_t3r_result *result, unsigned reg, uint32_t *value,
                     unsigned index)
{
    if (!shown(io, result, SWITCH_T3R_STEP_BEGIN(index)))
        return 0;
    *value = get(io, result, reg);
    if (result->stop)
        return 0;
    result->seen |= 1u << reg;
    derive(result, reg, index == T3R_READ_COUNT - 1);
    return shown(io, result, SWITCH_T3R_STEP_RESULT(index));
}

static void finish(const struct switch_t3r_io *io, struct switch_t3r_result *result)
{
    const int stopped = result->stop;
    int ok = 1;
    if (io->show)
        ok = io->show(io->context, T3R_STEP_COMPLETE, result);
    if (!ok && !stopped)
        result->stop = SWITCH_T3R_NOT_SHOWN;
}

unsigned switch_t3r_pwm_gate(uint32_t rst_devices_l, uint32_t clk_out_enb_l)
{
    if (!(clk_out_enb_l & SWITCH_T3R_CLOCK_PWM_BIT))
        return T3R_GATE_CLOCK_OFF;
    if (rst_devices_l & SWITCH_T3R_CLOCK_PWM_BIT)
        return T3R_GATE_IN_RESET;
    return T3R_GATE_OPEN;
}

/* Out of line on purpose: inlined, the compiler merges the two tests of the unused bits into one constant that looks like a
 * device address to the MMIO audit. */
__attribute__((noinline)) static uint32_t bits(uint32_t word, unsigned shift, uint32_t mask)
{
    return (word >> shift) & mask;
}

unsigned switch_t3r_pwm_classify(uint32_t word, unsigned *why, unsigned *duty)
{
    *why = T3R_PWM_WHY_NONE;
    *duty = 0;
    if (word == SWITCH_T3R_ALL_ONES) {
        *why = T3R_PWM_WHY_ALL_ONES;
        return T3R_PWM_UNKNOWN;
    }
    if (!(word & SWITCH_T3R_PWM_EN)) {
        if (!word)
            return T3R_PWM_DISABLED;
        *why = T3R_PWM_WHY_ENABLE_CLEAR_BITS;
        return T3R_PWM_UNKNOWN;
    }
    if (word & SWITCH_T3R_PWM_ABSOLUTE_0) {
        if (word == SWITCH_T3R_PWM_OFF_WORD)
            return T3R_PWM_OFF_HEKATE;
        *why = T3R_PWM_WHY_ABSOLUTE_0_BITS;
        return T3R_PWM_UNKNOWN;
    }
    if (bits(word, 25, 0x3fu) || bits(word, 0, 0xffffu)) {
        *why = T3R_PWM_WHY_OTHER_BITS;
        return T3R_PWM_UNKNOWN;
    }
    const uint32_t field = bits(word, 16, 0xffu);
    if (field > SWITCH_T3R_DUTY_MAX) {
        *why = T3R_PWM_WHY_OUT_OF_RANGE;
        return T3R_PWM_UNKNOWN;
    }
    if (field == SWITCH_T3R_DUTY_MAX) {
        *why = T3R_PWM_WHY_SPECIAL_ZERO_DUTY;
        return T3R_PWM_UNKNOWN;
    }
    *duty = SWITCH_T3R_DUTY_MAX - field;
    return T3R_PWM_DUTY;
}

#define HEKATE_OFF_LCD_GPIO2 0x74u
#define HEKATE_INIT_CAM1_PWDN 0x59u

static unsigned bit_is(uint32_t value, unsigned bit, unsigned wanted)
{
    return ((value >> bit) & 1u) == wanted ? T3R_EXPECT_MATCH : T3R_EXPECT_DIFFERS;
}

unsigned switch_t3r_expect(unsigned reg, uint32_t value)
{
    switch (reg) {
    case T3R_PWM_CSR_1: return value == SWITCH_T3R_PWM_OFF_WORD ? T3R_EXPECT_MATCH : T3R_EXPECT_DIFFERS;
    case T3R_PINMUX_LCD_GPIO2: return value == HEKATE_OFF_LCD_GPIO2 ? T3R_EXPECT_MATCH : T3R_EXPECT_DIFFERS;
    case T3R_PINMUX_CAM1_PWDN: return value == HEKATE_INIT_CAM1_PWDN ? T3R_EXPECT_MATCH : T3R_EXPECT_DIFFERS;
    case T3R_GPIO_A_OUT: return bit_is(value, 5, 0);
    case T3R_GPIO_CC_OUT: return bit_is(value, 4, 0);
    case T3R_GPIO_S_CNF: return bit_is(value, 7, 1);
    case T3R_GPIO_S_OE: return bit_is(value, 7, 0);
    case T3R_GPIO_V_CNF: return bit_is(value, 4, 0);
    default: return T3R_EXPECT_NONE;
    }
}

int switch_t3r_advanced(uint32_t first, uint32_t second)
{
    const uint32_t delta = second - first;
    return delta != 0 && delta < 0x80000000u;
}

static void derive(struct switch_t3r_result *result, unsigned reg, int second_tmrus)
{
    if (reg == T3R_CAR_CLK_OUT_ENB_L)
        result->gate = switch_t3r_pwm_gate(result->value[T3R_CAR_RST_DEVICES_L], result->value[T3R_CAR_CLK_OUT_ENB_L]);
    if (reg == T3R_PWM_CSR_1)
        result->pwm = switch_t3r_pwm_classify(result->value[T3R_PWM_CSR_1], &result->pwm_why, &result->pwm_duty);
    if (reg == T3R_TMRUS && second_tmrus)
        result->tmrus_advanced = switch_t3r_advanced(result->value[T3R_TMRUS], result->tmrus_second);
}

static int read_sequence(const struct switch_t3r_io *io, struct switch_t3r_result *result)
{
    for (unsigned index = 0; index < T3R_READ_COUNT; ++index) {
        const unsigned reg = switch_t3r_sequence(index);
        if (io->expired && io->expired(io->context)) {
            result->stop = SWITCH_T3R_TIME;
            return 0;
        }
        if (reg == T3R_PWM_CSR_1 && result->gate != T3R_GATE_OPEN) {
            if (!shown(io, result, T3R_STEP_SKIP_PWM))
                return 0;
            continue;
        }
        const int second_tmrus = index == T3R_READ_COUNT - 1;
        uint32_t *slot = second_tmrus ? &result->tmrus_second : &result->value[reg];
        if (!step_read(io, result, reg, slot, index))
            return 0;
        if (*slot == SWITCH_T3R_ALL_ONES && reg != T3R_TMRUS) {
            result->stop = SWITCH_T3R_STOP_ALL_ONES;
            return 0;
        }
    }
    return 1;
}

void switch_t3r_run(const struct switch_t3r_io *io, struct switch_t3r_result *result)
{
    static const unsigned summaries[] = {T3R_STEP_CAR, T3R_STEP_FAN, T3R_STEP_ROUTE, T3R_STEP_RAIL, T3R_STEP_TACH};
    /* A struct assignment would call memset, which the loader image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    if (shown(io, result, T3R_STEP_START) && read_sequence(io, result))
        for (unsigned i = 0; i < sizeof(summaries) / sizeof(summaries[0]); ++i)
            if (!shown(io, result, summaries[i]))
                break;
    finish(io, result);
}

uint64_t switch_t3r_settle_ticks(uint64_t cntfrq)
{
    return cntfrq == SWITCH_T3R_CNTFRQ ? cntfrq / 1000u * SWITCH_T3R_SETTLE_MS : 0;
}

uint64_t switch_t3r_budget_ticks(uint64_t cntfrq)
{
    return cntfrq == SWITCH_T3R_CNTFRQ ? cntfrq * SWITCH_T3R_READ_BUDGET_SECONDS : 0;
}
