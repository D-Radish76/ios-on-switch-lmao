#ifndef T4A0_GATE_H
#define T4A0_GATE_H

#include <stdint.h>

enum t4a_status { T4A_OK, T4A_FAULT, T4A_HANG };

typedef struct t4a_io {
    void *context;
    int (*read)(void *context, uint32_t address, unsigned width, uint64_t *value);
    int (*write)(void *context, uint32_t address, unsigned width, uint64_t value);
} t4a_io;

enum t4a_state {
    T4A_INIT, T4A_CHECKED, T4A_PTV_SET, T4A_CONFIG_SET, T4A_ARMED,
    T4A_STOPPING, T4A_STOPPED, T4A_STOP_FAILED, T4A_REFUSED, T4A_HUNG,
};

enum t4a_cause {
    T4A_CAUSE_NONE, T4A_CAUSE_REFUSAL, T4A_CAUSE_READBACK, T4A_CAUSE_WRITE_FAULT,
    T4A_CAUSE_READ_FAULT, T4A_CAUSE_NOT_IDLE, T4A_CAUSE_SECOND_ARM, T4A_CAUSE_BAD_PARAMS,
};

enum t4a_verdict { T4A_ALLOWED, T4A_DENIED, T4A_REFUSED_RUN, T4A_HUNG_RUN };

#define T4A_TMRUS 0x60005010u
#define T4A_CONFIG 0x60005100u
#define T4A_STATUS 0x60005104u
#define T4A_COMMAND 0x60005108u
#define T4A_UNLOCK 0x6000510cu
#define T4A_UNLOCK_KEY 0xc45au
#define T4A_START 1u
#define T4A_DISABLE 2u

typedef struct t4a_params {
    uint32_t source;
    uint32_t ptv_address;
    uint32_t ptv_word;
    uint32_t config_word;
} t4a_params;

typedef struct t4a_gate {
    t4a_params params;
    enum t4a_state state;
    enum t4a_cause cause;
    unsigned stop_step;
    unsigned writes;
    unsigned start_writes;
    unsigned denied;
    int read_back_ok;
    int line_drawn;
    int *once;
} t4a_gate;

/* Timer sources are candidates only: 5 is the mainline choice, 0 and 7 are the other pairings found in the sources. */
int t4a_params_make(t4a_params *params, uint32_t source, uint32_t period_field, uint32_t ptv_count);

void t4a_gate_init(t4a_gate *gate, const t4a_params *params, int *once_per_run);
int t4a_gate_checked(t4a_gate *gate, const t4a_io *io);
void t4a_gate_draw(t4a_gate *gate);
int t4a_gate_read(t4a_gate *gate, const t4a_io *io, uint32_t address, unsigned width, uint64_t *value);
int t4a_gate_write(t4a_gate *gate, const t4a_io *io, uint32_t address, unsigned width, uint64_t value);
int t4a_gate_verify(t4a_gate *gate, const t4a_io *io);

int t4a_gate_is_terminal(const t4a_gate *gate);
int t4a_gate_may_park(const t4a_gate *gate);

#endif
