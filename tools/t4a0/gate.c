#include "gate.h"

#define PTV_ENABLE (1u << 31)
#define PTV_PERIODIC (1u << 30)
#define PTV_COUNT_MASK 0x1fffffffu
#define CONFIG_PMC2CAR (1u << 15)

static const struct { uint32_t source; uint32_t ptv_address; } candidates[] = {
    {0, 0x60005088u}, {5, 0x60005060u}, {7, 0x60005070u},
};

int t4a_params_make(t4a_params *params, uint32_t source, uint32_t period_field, uint32_t ptv_count)
{
    if (period_field < 1 || period_field > 0xff || ptv_count < 1 || ptv_count > PTV_COUNT_MASK)
        return -1;
    for (unsigned i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        if (candidates[i].source != source)
            continue;
        params->source = source;
        params->ptv_address = candidates[i].ptv_address;
        params->ptv_word = PTV_ENABLE | PTV_PERIODIC | ptv_count;
        params->config_word = source | (period_field << 4) | CONFIG_PMC2CAR;
        return 0;
    }
    return -1;
}

int t4a_gate_is_terminal(const t4a_gate *gate)
{
    return gate->state == T4A_STOPPED || gate->state == T4A_STOP_FAILED || gate->state == T4A_REFUSED ||
           gate->state == T4A_HUNG;
}

int t4a_gate_may_park(const t4a_gate *gate)
{
    return gate->state == T4A_ARMED && gate->read_back_ok;
}

void t4a_gate_init(t4a_gate *gate, const t4a_params *params, int *once_per_run)
{
    *gate = (t4a_gate){.params = *params, .state = T4A_INIT, .once = once_per_run};
    if (*once_per_run) {
        gate->state = T4A_REFUSED;
        gate->cause = T4A_CAUSE_SECOND_ARM;
    }
}

/* The only store in this file. */
static int emit(t4a_gate *gate, const t4a_io *io, uint32_t address, uint32_t value)
{
    gate->writes++;
    return io->write(io->context, address, 32, value);
}

static int hang(t4a_gate *gate)
{
    gate->state = T4A_HUNG;
    gate->cause = T4A_CAUSE_NONE;
    return T4A_HUNG_RUN;
}

static int stop_sequence(t4a_gate *gate, const t4a_io *io, enum t4a_cause cause)
{
    const struct { uint32_t address; uint32_t value; } steps[] = {
        {T4A_UNLOCK, T4A_UNLOCK_KEY}, {T4A_COMMAND, T4A_DISABLE}, {gate->params.ptv_address, 0},
    };
    uint64_t seen;
    int status;

    gate->state = T4A_STOPPING;
    gate->cause = cause;
    for (gate->stop_step = 0; gate->stop_step < 3; gate->stop_step++) {
        status = emit(gate, io, steps[gate->stop_step].address, steps[gate->stop_step].value);
        if (status == T4A_HANG)
            return hang(gate);
        if (status != T4A_OK) {
            gate->state = T4A_STOP_FAILED;
            return T4A_REFUSED_RUN;
        }
    }
    status = io->read(io->context, gate->params.ptv_address, 32, &seen);
    if (status == T4A_HANG)
        return hang(gate);
    gate->state = status == T4A_OK && seen == 0 ? T4A_STOPPED : T4A_STOP_FAILED;
    return T4A_REFUSED_RUN;
}

static int refuse(t4a_gate *gate, const t4a_io *io, enum t4a_cause cause)
{
    switch (gate->state) {
    case T4A_PTV_SET:
    case T4A_CONFIG_SET:
        return stop_sequence(gate, io, cause);
    case T4A_ARMED:
        if (gate->read_back_ok) {
            gate->denied++;
            return T4A_DENIED;
        }
        return stop_sequence(gate, io, cause);
    case T4A_INIT:
    case T4A_CHECKED:
        gate->state = T4A_REFUSED;
        gate->cause = cause;
        return T4A_REFUSED_RUN;
    default:
        gate->denied++;
        return T4A_DENIED;
    }
}

static int read_allowed(const t4a_gate *gate, uint32_t address)
{
    return address == T4A_TMRUS || address == gate->params.ptv_address || address == T4A_CONFIG ||
           address == T4A_STATUS;
}

int t4a_gate_read(t4a_gate *gate, const t4a_io *io, uint32_t address, unsigned width, uint64_t *value)
{
    int status;

    if (t4a_gate_is_terminal(gate) && gate->state != T4A_STOPPED && gate->state != T4A_REFUSED)
        return T4A_DENIED;
    if (width != 32 || !read_allowed(gate, address))
        return refuse(gate, io, T4A_CAUSE_REFUSAL);
    status = io->read(io->context, address, width, value);
    if (status == T4A_HANG)
        return hang(gate);
    if (status != T4A_OK)
        return refuse(gate, io, T4A_CAUSE_READ_FAULT);
    return T4A_ALLOWED;
}

int t4a_gate_checked(t4a_gate *gate, const t4a_io *io)
{
    uint64_t ptv, config, status;
    int verdict;

    if (gate->state != T4A_INIT)
        return refuse(gate, io, T4A_CAUSE_REFUSAL);
    if ((verdict = t4a_gate_read(gate, io, gate->params.ptv_address, 32, &ptv)) != T4A_ALLOWED ||
        (verdict = t4a_gate_read(gate, io, T4A_CONFIG, 32, &config)) != T4A_ALLOWED ||
        (verdict = t4a_gate_read(gate, io, T4A_STATUS, 32, &status)) != T4A_ALLOWED)
        return verdict;
    if (ptv || config || status)
        return refuse(gate, io, T4A_CAUSE_NOT_IDLE);
    gate->state = T4A_CHECKED;
    return T4A_ALLOWED;
}

void t4a_gate_draw(t4a_gate *gate)
{
    gate->line_drawn = 1;
}

int t4a_gate_write(t4a_gate *gate, const t4a_io *io, uint32_t address, unsigned width, uint64_t value)
{
    uint32_t want_address, want_value;
    enum t4a_state next;
    int status;

    if (t4a_gate_is_terminal(gate) || gate->state == T4A_STOPPING) {
        gate->denied++;
        return T4A_DENIED;
    }
    switch (gate->state) {
    case T4A_CHECKED:
        want_address = gate->params.ptv_address, want_value = gate->params.ptv_word, next = T4A_PTV_SET;
        break;
    case T4A_PTV_SET:
        want_address = T4A_CONFIG, want_value = gate->params.config_word, next = T4A_CONFIG_SET;
        break;
    case T4A_CONFIG_SET:
        want_address = T4A_COMMAND, want_value = T4A_START, next = T4A_ARMED;
        break;
    default:
        return refuse(gate, io, T4A_CAUSE_REFUSAL);
    }
    if (width != 32 || address != want_address || value != want_value || !gate->line_drawn ||
        (gate->state != T4A_CHECKED && !gate->read_back_ok) || (next == T4A_ARMED && *gate->once))
        return refuse(gate, io, T4A_CAUSE_REFUSAL);

    gate->state = next;
    gate->read_back_ok = 0;
    gate->line_drawn = 0;
    if (next == T4A_ARMED) {
        gate->start_writes++;
        *gate->once = 1;
    }
    status = emit(gate, io, address, (uint32_t)value);
    if (status == T4A_HANG)
        return hang(gate);
    if (status != T4A_OK)
        return stop_sequence(gate, io, T4A_CAUSE_WRITE_FAULT);
    return T4A_ALLOWED;
}

int t4a_gate_verify(t4a_gate *gate, const t4a_io *io)
{
    uint64_t ptv, config;
    int status;

    if (gate->state != T4A_PTV_SET && gate->state != T4A_CONFIG_SET && gate->state != T4A_ARMED) {
        gate->denied++;
        return T4A_DENIED;
    }
    status = io->read(io->context, gate->params.ptv_address, 32, &ptv);
    if (status == T4A_OK && gate->state != T4A_PTV_SET)
        status = io->read(io->context, T4A_CONFIG, 32, &config);
    if (status == T4A_HANG)
        return hang(gate);
    if (status != T4A_OK)
        return stop_sequence(gate, io, T4A_CAUSE_READ_FAULT);
    if (ptv != gate->params.ptv_word || (gate->state != T4A_PTV_SET && config != gate->params.config_word))
        return stop_sequence(gate, io, T4A_CAUSE_READBACK);
    gate->read_back_ok = 1;
    return T4A_ALLOWED;
}
