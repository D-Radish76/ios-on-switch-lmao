#include "sim.h"

void t4a_sim_init(t4a_sim *sim, uint32_t ptv_address)
{
    *sim = (t4a_sim){.ptv_address = ptv_address};
}

void t4a_sim_inject(t4a_sim *sim, unsigned access_index, enum t4a_fault_kind kind)
{
    if (sim->injected < 2) {
        sim->fault_index[sim->injected] = access_index;
        sim->fault_kind[sim->injected++] = kind;
    }
}

int t4a_sim_counting(const t4a_sim *sim)
{
    return sim->started && !sim->disabled && sim->ptv != 0;
}

static enum t4a_fault_kind next_fault(t4a_sim *sim)
{
    unsigned index = sim->accesses++;
    for (unsigned i = 0; i < sim->injected; i++)
        if (index == sim->fault_index[i])
            return sim->fault_kind[i];
    return T4A_FAULT_NONE;
}

static void record(t4a_sim *sim, int is_write, uint32_t address, unsigned width, uint64_t value, int status)
{
    if (sim->logged < T4A_LOG_MAX)
        sim->log[sim->logged++] = (t4a_access){is_write, address, width, value, status};
}

static int sim_read(void *context, uint32_t address, unsigned width, uint64_t *value)
{
    t4a_sim *sim = context;
    enum t4a_fault_kind fault = next_fault(sim);
    uint64_t seen = 0;
    int status = T4A_OK;

    if (sim->hung)
        status = T4A_HANG;
    else if (fault == T4A_FAULT_HANG)
        sim->hung = 1, status = T4A_HANG;
    else if (fault == T4A_FAULT_BUS)
        status = T4A_FAULT;
    else {
        if (address == sim->ptv_address)
            seen = sim->ptv;
        else if (address == T4A_CONFIG)
            seen = sim->config;
        else if (address == T4A_STATUS)
            seen = sim->status;
        else if (address == T4A_TMRUS)
            seen = 0x1000u + 100u * sim->accesses;
        if (fault == T4A_FAULT_CORRUPT)
            seen ^= 1;
        *value = seen;
    }
    record(sim, 0, address, width, seen, status);
    return status;
}

static void apply(t4a_sim *sim, uint32_t address, uint64_t value)
{
    if (address == sim->ptv_address)
        sim->ptv = (uint32_t)value;
    else if (address == T4A_CONFIG)
        sim->config = (uint32_t)value;
    else if (address == T4A_UNLOCK)
        sim->unlocked = value == T4A_UNLOCK_KEY;
    else if (address == T4A_COMMAND && value == T4A_START) {
        sim->started = 1;
        sim->disabled = 0;
        sim->starts++;
    } else if (address == T4A_COMMAND && value == T4A_DISABLE && sim->unlocked)
        sim->disabled = 1;
    else
        sim->stray_writes++;
}

static int sim_write(void *context, uint32_t address, unsigned width, uint64_t value)
{
    t4a_sim *sim = context;
    enum t4a_fault_kind fault = next_fault(sim);
    int status = T4A_OK;

    if (sim->hung)
        status = T4A_HANG;
    else if (fault == T4A_FAULT_HANG)
        sim->hung = 1, status = T4A_HANG;
    else if (fault == T4A_FAULT_BUS)
        status = T4A_FAULT;
    else if (fault != T4A_FAULT_DROP)
        apply(sim, address, fault == T4A_FAULT_CORRUPT ? value ^ 1 : value);
    record(sim, 1, address, width, value, status);
    return status;
}

t4a_io t4a_sim_io(t4a_sim *sim)
{
    return (t4a_io){.context = sim, .read = sim_read, .write = sim_write};
}
