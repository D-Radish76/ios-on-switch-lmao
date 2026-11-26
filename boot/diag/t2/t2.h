#ifndef SWITCH_DIAG_T2_H
#define SWITCH_DIAG_T2_H
#include <stdint.h>

/* T2's sensor sequence (ERISTA_T2_DESIGN.md), one TMP451 conversion, over a bus that does single register transfers. The
 * I2C1 controller setup is T1 v3's and is not here. Prepared on the host; no image links this yet. */

enum switch_t2_reg {
    T2_LOCAL_HIGH = 0x00,
    T2_REMOTE_HIGH = 0x01,
    T2_STATUS = 0x02,
    T2_CONFIG = 0x03,
    T2_REMOTE_LOW = 0x10,
    T2_OFFSET_HIGH = 0x11,
    T2_OFFSET_LOW = 0x12,
    T2_LOCAL_LOW = 0x15,
    T2_ONE_SHOT = 0x0f,
    T2_MANUFACTURER = 0xfe,
};

enum {
    T2_MANUFACTURER_TI = 0x55,
    T2_CONFIG_SHUTDOWN = 0xc0,
    T2_STATUS_BUSY = 0x80,
    T2_STATUS_OPEN = 0x04,
    T2_MIN_US = 31000,
    T2_TYPICAL_US = 34000,
    T2_FIRST_POLL_US = 2000,
    T2_POLL_STEP_US = 5000,
    T2_BOUND_US = 100000,
};

enum {
    T2_STOP_NONE = 0,
    T2_STOP_READ_FAILED = 0x720,
    T2_STOP_ID = 0x721,
    T2_STOP_CONFIG = 0x722,
    T2_STOP_BUSY_BEFORE = 0x723,
    T2_STOP_WRITE_FAILED = 0x724,
    T2_STOP_TIMEOUT = 0x725,
};

struct switch_t2_bus {
    void *context;
    int (*read)(void *context, unsigned reg, uint8_t *value); /* 0 on success */
    int (*write)(void *context, unsigned reg, uint8_t value); /* 0 on success; the only sensor write is T2_ONE_SHOT */
    uint32_t (*now_us)(void *context);
    void (*wait_us)(void *context, uint32_t us);
};

struct switch_t2_sample {
    uint8_t remote_high, remote_low, local_high, local_low;
};

struct switch_t2_result {
    int stop;
    unsigned writes;
    uint8_t id, config_before, config_after, status_before, offset_high, offset_low;
    struct switch_t2_sample old_sample, new_sample;
    uint8_t status_last;
    int busy_seen;
    uint32_t busy_first_us; /* from the write, 0 when never seen */
    uint32_t done_us;       /* from the write to the first status without BUSY, 0 when not reached */
    int inconsistent;       /* the two remote high reads differ */
    int not_in_shutdown;    /* SD cleared after the run */
};

void switch_t2_run(const struct switch_t2_bus *bus, struct switch_t2_result *result);

/* A reading in sixteenths of a degree, from the high byte and the low byte's upper nibble (standard range). */
int switch_t2_sixteenths(uint8_t high, uint8_t low);
/* The standard range clamps at 0 and 127 C, so those two high bytes say nothing but "at or past the edge". */
int switch_t2_at_range_edge(uint8_t high);
#endif
