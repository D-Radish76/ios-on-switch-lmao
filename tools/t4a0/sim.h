#ifndef T4A0_SIM_H
#define T4A0_SIM_H

#include "gate.h"

enum t4a_fault_kind { T4A_FAULT_NONE, T4A_FAULT_BUS, T4A_FAULT_HANG, T4A_FAULT_DROP, T4A_FAULT_CORRUPT, T4A_FAULT_KINDS };

#define T4A_LOG_MAX 64

typedef struct t4a_access {
    int is_write;
    uint32_t address;
    unsigned width;
    uint64_t value;
    int status;
} t4a_access;

/* A host model of the TKE watchdog block. It is a struct in memory; nothing here reaches a device. */
typedef struct t4a_sim {
    uint32_t ptv_address;
    uint32_t ptv, config, status;
    int unlocked, started, disabled;
    unsigned starts, stray_writes;
    unsigned accesses;
    unsigned fault_index[2];
    enum t4a_fault_kind fault_kind[2];
    unsigned injected;
    int hung;
    unsigned logged;
    t4a_access log[T4A_LOG_MAX];
} t4a_sim;

void t4a_sim_init(t4a_sim *sim, uint32_t ptv_address);
/* Up to two faults per run, each on one numbered access. */
void t4a_sim_inject(t4a_sim *sim, unsigned access_index, enum t4a_fault_kind kind);
t4a_io t4a_sim_io(t4a_sim *sim);
int t4a_sim_counting(const t4a_sim *sim);

#endif
