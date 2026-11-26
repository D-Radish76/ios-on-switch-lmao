#ifndef SWITCH_DIAG_T2_BUS_H
#define SWITCH_DIAG_T2_BUS_H
#include <stdint.h>
#include "../t2/t2.h"
#include "t1v3.h"

/* The T2 bus layer: single TMP451 register transfers over I2C1 at the divisor T1 v3 applied, written against the T1 v3
 * register table. Its allowlist is T1 v3's with more values (ERISTA_T2_DESIGN.md, "All writes"). */

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
    const struct switch_t1v3_io *io;
    int stop;
    unsigned writes;
    unsigned transfers;
};

int switch_t2_may_write(unsigned reg, uint32_t value);

/* The bus the sequence in t2.c runs over. The caller owns hw and keeps it alive for the run. */
void switch_t2_hw_bus(struct switch_t2_hw *hw, struct switch_t2_bus *bus);
#endif
