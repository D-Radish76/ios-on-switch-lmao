#ifndef SWITCH_DIAG_T2_BUS_H
#define SWITCH_DIAG_T2_BUS_H
#include <stdint.h>
#include "../t2/t2.h"
#include "t2_regs.h"

/* The T2 bus layer: single TMP451 register transfers over I2C1, on T2's own register table and allowlist. */

enum {
    T2_BUS_DENIED = 0x730,
    T2_BUS_NOT_STARTED = 0x731,
    T2_BUS_BUSY_TIMEOUT = 0x732,
    T2_BUS_NOACK = 0x733,
    T2_BUS_LOAD_TIMEOUT = 0x734,
    T2_BUS_LOAD_UNEXPECTED = 0x735,
    T2_BUS_COUNTER_STALLED = 0x736,
};

struct switch_t2_hw {
    const struct switch_t2_io *io;
    int stop;
    unsigned writes;
    unsigned transfers;
};

/* The controller steps both the prelude and the transfers use. Once hw->stop is set, put writes nothing. */
void switch_t2_hw_stop(struct switch_t2_hw *hw, int code);
uint32_t switch_t2_hw_get(struct switch_t2_hw *hw, unsigned reg);
void switch_t2_hw_put(struct switch_t2_hw *hw, unsigned reg, uint32_t value);
uint64_t switch_t2_hw_now(struct switch_t2_hw *hw);
int switch_t2_hw_wait_us(struct switch_t2_hw *hw, uint32_t us);
/* Writes CONFIG_LOAD with value, then waits for bit 0 to clear. 1 on success. */
int switch_t2_hw_load(struct switch_t2_hw *hw, uint32_t value, int load_stop);

/* The bus the sequence in t2.c runs over. The caller owns hw and keeps it alive for the run. */
void switch_t2_hw_bus(struct switch_t2_hw *hw, struct switch_t2_bus *bus);
#endif
