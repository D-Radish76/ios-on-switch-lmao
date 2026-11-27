#include "sequence.h"

static int step(t4a_gate *gate, const t4a_io *io, uint32_t address, uint32_t value)
{
    int verdict;

    t4a_gate_draw(gate);
    verdict = t4a_gate_write(gate, io, address, 32, value);
    if (verdict != T4A_ALLOWED)
        return verdict;
    return t4a_gate_verify(gate, io);
}

int t4a_model_run(t4a_gate *gate, const t4a_io *io)
{
    int verdict = t4a_gate_checked(gate, io);

    if (verdict != T4A_ALLOWED)
        return verdict;
    if ((verdict = step(gate, io, gate->params.ptv_address, gate->params.ptv_word)) != T4A_ALLOWED ||
        (verdict = step(gate, io, T4A_CONFIG, gate->params.config_word)) != T4A_ALLOWED)
        return verdict;
    return step(gate, io, T4A_COMMAND, T4A_START);
}
