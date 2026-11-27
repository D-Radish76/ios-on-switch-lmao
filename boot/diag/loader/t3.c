#include "t3.h"

static void stop(struct switch_t3_result *result, int code)
{
    if (!result->stop)
        result->stop = code;
}

/* Drawing and settling take time with the rail on, so the rail limit is looked at after every step. */
static int shown(const struct switch_t3_io *io, struct switch_t3_result *result, unsigned step)
{
    const int drawn = !io->show || io->show(io->context, step, result);
    if (!drawn)
        stop(result, SWITCH_T3_NOT_SHOWN);
    if (result->rail != T3_RAIL_OFF_CONFIRMED && io->rail_expired(io->context))
        stop(result, SWITCH_T3_RAIL_TIME);
    return drawn;
}

static int turns_on(unsigned write)
{
    return write == T3_W_RAIL_ON || write == T3_W_PWM_DUTY || write == T3_W_ROUTE_PWM;
}

/* No write that turns anything on is sent after a stop or once the rail limit is spent. Writes that turn things off are
 * never held back by either. */
static int on_write_allowed(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    if (!result->stop && result->rail != T3_RAIL_OFF_CONFIRMED && io->rail_expired(io->context))
        stop(result, SWITCH_T3_RAIL_TIME);
    return !result->stop;
}

/* Sends a write through the allowlist check, reads the register back and records whether it matched. A mismatch stops the
 * run but never the writes that follow it in the off sequence. */
static int write_checked(const struct switch_t3_io *io, struct switch_t3_result *result, unsigned write)
{
    const unsigned reg = switch_t3_write_reg(write);
    const uint32_t value = switch_t3_write_value(write);
    if (!switch_t3_may_write(reg, value) || !switch_t3_may_read(reg, T3_WIDTH)) {
        stop(result, SWITCH_T3_DENIED);
        return 0;
    }
    if (turns_on(write) && !on_write_allowed(io, result))
        return 0;
    if (write == T3_W_RAIL_ON) {
        result->rail = T3_RAIL_MAYBE_ON;
        io->rail_mark(io->context);
    }
    result->issued |= 1u << write;
    ++result->writes;
    io->write(io->context, reg, value);
    ++result->reads;
    result->readback[write] = io->read(io->context, reg);
    if (write == T3_O_RAIL_OFF) {
        if (result->readback[write] == SWITCH_T3_RAIL_OFF_WORD)
            result->rail = T3_RAIL_OFF_CONFIRMED;
        else if (!(result->readback[write] & SWITCH_T3_RAIL_ON_WORD))
            result->rail = T3_RAIL_UNKNOWN;
    }
    if (result->readback[write] != value) {
        result->mismatched |= 1u << write;
        stop(result, SWITCH_T3_READBACK);
        return 0;
    }
    return 1;
}

static int read_state(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    for (unsigned reg = 0; reg < T3_REG_COUNT; ++reg) {
        if (io->read_expired(io->context)) {
            stop(result, SWITCH_T3_TIME);
            return 0;
        }
        if (!switch_t3_may_read(reg, T3_WIDTH)) {
            stop(result, SWITCH_T3_DENIED);
            return 0;
        }
        ++result->reads;
        result->value[reg] = io->read(io->context, reg);
        if (result->value[reg] == SWITCH_T3_ALL_ONES) {
            stop(result, SWITCH_T3_STOP_ALL_ONES);
            return 0;
        }
    }
    result->baseline_level = result->value[T3_GPIO_S_IN] >> SWITCH_T3_TACH_BIT & 1u;
    result->state = switch_t3_state_check(result->value);
    if (result->state != T3_STATE_OK)
        stop(result, SWITCH_T3_STATE);
    return result->state == T3_STATE_OK;
}

static int rail_ok(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    if (result->rail != T3_RAIL_OFF_CONFIRMED && io->rail_expired(io->context))
        stop(result, SWITCH_T3_RAIL_TIME);
    return !result->stop;
}

/* Undoes what was sent, in the order PWM, rail, pad. A readback that differs, a line that did not draw and a rail limit
 * that is spent change nothing here: every undo still runs. */
static void off_sequence(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    static const struct {
        unsigned off;
        unsigned on;
    } undo[] = {{T3_O_PWM_OFF, T3_W_PWM_DUTY}, {T3_O_RAIL_OFF, T3_W_RAIL_ON}, {T3_O_PARK_PAD, T3_W_ROUTE_PWM}};
    for (unsigned i = 0; i < sizeof(undo) / sizeof(undo[0]); ++i)
        if (result->issued & 1u << undo[i].on)
            write_checked(io, result, undo[i].off);
    for (unsigned retry = 0; retry < SWITCH_T3_OFF_RETRIES && result->rail != T3_RAIL_OFF_CONFIRMED; ++retry)
        write_checked(io, result, T3_O_RAIL_OFF);
}

static int begin(const struct switch_t3_io *io, struct switch_t3_result *result, unsigned phase)
{
    return !result->stop && shown(io, result, SWITCH_T3_STEP_BEGIN(phase)) && !result->stop;
}

static void done(const struct switch_t3_io *io, struct switch_t3_result *result, unsigned phase)
{
    shown(io, result, SWITCH_T3_STEP_DONE(phase));
}

static void idle_and_rail(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    if (begin(io, result, T3_PHASE_IDLE)) {
        switch_t3_count_window(io, result, T3_WINDOW_IDLE, SWITCH_T3_WINDOW_MS);
        done(io, result, T3_PHASE_IDLE);
    }
    if (begin(io, result, T3_PHASE_RAIL)) {
        if (write_checked(io, result, T3_W_RAIL_ON) && rail_ok(io, result))
            switch_t3_count_window(io, result, T3_WINDOW_RAIL, SWITCH_T3_WINDOW_MS);
        done(io, result, T3_PHASE_RAIL);
    }
}

static void spin(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    if (!rail_ok(io, result) || !begin(io, result, T3_PHASE_SPIN))
        return;
    if (write_checked(io, result, T3_W_PWM_DUTY) && write_checked(io, result, T3_W_ROUTE_PWM) && rail_ok(io, result)) {
        switch_t3_count_window(io, result, T3_WINDOW_SPINUP, SWITCH_T3_SPINUP_MS);
        if (rail_ok(io, result))
            switch_t3_count_window(io, result, T3_WINDOW_SPIN, SWITCH_T3_WINDOW_MS);
    }
    done(io, result, T3_PHASE_SPIN);
}

static void finish(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    if (result->rail != T3_RAIL_OFF_CONFIRMED)
        result->stop = SWITCH_T3_RAIL_NOT_OFF;
    else if (!result->stop) {
        result->verdict = switch_t3_verdict(result->window);
        if (result->verdict == T3_VERDICT_NO_EDGES)
            result->stop = SWITCH_T3_NO_EDGES;
        else if (result->verdict != T3_VERDICT_PASS)
            result->stop = SWITCH_T3_SAMPLES;
    }
    shown(io, result, SWITCH_T3_STEP_COMPLETE);
}

void switch_t3_run(const struct switch_t3_io *io, struct switch_t3_result *result)
{
    /* A struct assignment would call memset, which the loader image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    if (shown(io, result, SWITCH_T3_STEP_START) && begin(io, result, T3_PHASE_STATE)) {
        read_state(io, result);
        done(io, result, T3_PHASE_STATE);
    }
    if (!result->stop) {
        idle_and_rail(io, result);
        spin(io, result);
    }
    if (result->issued) {
        off_sequence(io, result);
        shown(io, result, SWITCH_T3_STEP_DONE(T3_PHASE_OFF));
        if (!result->stop && begin(io, result, T3_PHASE_STOPPED)) {
            switch_t3_count_window(io, result, T3_WINDOW_STOPPED, SWITCH_T3_WINDOW_MS);
            done(io, result, T3_PHASE_STOPPED);
        }
    }
    finish(io, result);
}
