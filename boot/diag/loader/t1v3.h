#ifndef SWITCH_DIAG_T1V3_H
#define SWITCH_DIAG_T1V3_H
#include <stdint.h>

/* Every register T1 v3 may touch, by name. The CAR is read and never written. */
enum switch_t1v3_reg {
    T1V3_CAR_RST_DEVICES_L,
    T1V3_CAR_CLK_OUT_ENB_L,
    T1V3_CAR_CLK_SOURCE_I2C1,
    T1V3_CAR_PLLP_BASE,
    T1V3_I2C1_CNFG,
    T1V3_I2C1_CMD_ADDR0,
    T1V3_I2C1_CMD_DATA1,
    T1V3_I2C1_STATUS,
    T1V3_I2C1_INT_STATUS,
    T1V3_I2C1_CLK_DIVISOR,
    T1V3_I2C1_CONFIG_LOAD,
    T1V3_I2C1_TIMING_0,
    T1V3_I2C1_TIMING_1,
    T1V3_REG_COUNT
};

#define SWITCH_T1V3_SOURCE_EXPECTED 0x00000003u
#define SWITCH_T1V3_DIVISOR_BEFORE  0x00050001u
#define SWITCH_T1V3_DIVISOR_AFTER   0x00ff0001u
/* T210 PLLP_BASE fields: M bits 7:0, N bits 17:10, P bits 24:20. The row of Linux's table for 38.4 MHz to 408 MHz. */
#define SWITCH_T1V3_PLLP_M(word)    ((word) & 0xffu)
#define SWITCH_T1V3_PLLP_N(word)    (((word) >> 10) & 0xffu)
#define SWITCH_T1V3_PLLP_P(word)    (((word) >> 20) & 0x1fu)
#define SWITCH_T1V3_PLLP_M_EXPECTED 8u
#define SWITCH_T1V3_PLLP_N_EXPECTED 85u
#define SWITCH_T1V3_PLLP_P_EXPECTED 1u
/* CONFIG_LOAD at rest on this silicon (T1-R). Bit 2 is hekate's TIMEOUT_CONFIG_LOAD; it does not clear by itself here. */
#define SWITCH_T1V3_LOAD_REST       0x00000004u
#define SWITCH_T1V3_LOAD_APPLY      0x01u
#define SWITCH_T1V3_LOAD_TRANSFER   0x25u
#define SWITCH_T1V3_ID_REGISTER     0xfeu
#define SWITCH_T1V3_ID_VALUE        0x55u
#define SWITCH_T1V3_TRANSACTIONS    2u

#define SWITCH_T1V3_LOAD_TIMEOUT_US 1000u
#define SWITCH_T1V3_BUSY_TIMEOUT_US 100000u
#define SWITCH_T1V3_IDLE_SAMPLES    3u
#define SWITCH_T1V3_IDLE_SPACING_US 100u
/* After go, BUSY must rise within this time. A transaction that never shows BUSY is not told apart from one that never
 * started, so it is not counted as done. */
#define SWITCH_T1V3_START_WINDOW_US 1000u
/* Every wait and poll loop also ends after this many iterations, whatever the counter says. */
#define SWITCH_T1V3_POLL_LIMIT      1000000u

/* Why T1 v3 stopped. 0 is a completed run; any other code ends the work at once, with no retry and no restore. */
enum {
    SWITCH_T1V3_I2C_CLOCK_OFF = 0x601,
    SWITCH_T1V3_I2C_IN_RESET = 0x602,
    SWITCH_T1V3_SOURCE_UNEXPECTED = 0x603,
    SWITCH_T1V3_BUS_BUSY = 0x604,
    SWITCH_T1V3_DIVISOR_UNEXPECTED = 0x605,
    SWITCH_T1V3_TIMING_ZERO = 0x606,
    SWITCH_T1V3_TIMING_UNEXPECTED = 0x607,
    SWITCH_T1V3_TIMING_UNREADABLE = 0x608,
    SWITCH_T1V3_IDLE_FAILED = 0x609,
    SWITCH_T1V3_CONFIG_LOAD_TIMEOUT = 0x60a,
    SWITCH_T1V3_DIVISOR_READBACK = 0x60b,
    SWITCH_T1V3_BUSY_TIMEOUT = 0x60c,
    SWITCH_T1V3_NOACK = 0x60d,
    SWITCH_T1V3_ID_MISMATCH = 0x60e,
    SWITCH_T1V3_NOT_IDLE = 0x60f,
    SWITCH_T1V3_DENIED = 0x610,
    SWITCH_T1V3_NOT_ERISTA = 0x611,
    SWITCH_T1V3_COUNTER_STALLED = 0x612,
    SWITCH_T1V3_INT_PENDING = 0x613,
    SWITCH_T1V3_INT_CHANGED = 0x614,
    SWITCH_T1V3_LOAD_BEFORE_UNEXPECTED = 0x615,
    SWITCH_T1V3_LOAD_BEFORE_PENDING = 0x616,
    SWITCH_T1V3_LOAD_AFTER_UNEXPECTED = 0x617,
    SWITCH_T1V3_NOT_STARTED = 0x618,
    SWITCH_T1V3_STALE_DATA = 0x619,
    SWITCH_T1V3_TIMING_NOT_PINNED = 0x61a,
    SWITCH_T1V3_PLLP_UNEXPECTED = 0x61b,
};

/* The two timing words that T1 v3 may go on from. Nothing read says what they hold on this silicon (Linux writes TIMING_0
 * itself and leaves TIMING_1 at the chip default), so both are 0 until a read-only run has shown them; 0 stops the run. */
struct switch_t1v3_io {
    void *context;
    uint32_t timing_0_pin, timing_1_pin;
    uint32_t (*read)(void *context, unsigned reg);
    void (*write)(void *context, unsigned reg, uint32_t value);
    uint64_t (*now_us)(void *context);
};

/* One transaction as the screen reports it. started_us is go to the first poll that saw BUSY, elapsed_us is go to the
 * poll that saw it clear. Both are reference values, not a measurement of the bus. cnfg and int_status are read once
 * after the transaction and decide nothing. */
struct switch_t1v3_transaction {
    uint32_t started_us, elapsed_us;
    uint32_t cnfg, int_status;
    int started;
};

struct switch_t1v3_result {
    int stop;
    unsigned writes;
    unsigned transfers;
    uint32_t rst_devices_l, clk_out_enb_l, source_i2c1, pllp_base;
    uint32_t cnfg, status, int_status, divisor, config_load, timing_0, timing_1;
    uint32_t load_after, divisor_after, status_load, int_status_load, status_after, int_status_after;
    uint32_t load_transfer[SWITCH_T1V3_TRANSACTIONS], data_before_read;
    struct switch_t1v3_transaction transaction[SWITCH_T1V3_TRANSACTIONS];
    uint8_t id;
    int have_id;
};

/* Allowlist. The address of register reg, or 0 when reg is not a T1 v3 register. */
uint32_t switch_t1v3_address(unsigned reg);
int switch_t1v3_may_read(unsigned reg);
/* A write is allowed for one register and one of its listed values. */
int switch_t1v3_may_write(unsigned reg, uint32_t value);

/* The whole run. Reads CAR first and touches no I2C1 register unless the clock is on and the block is out of reset. */
void switch_t1v3_run(const struct switch_t1v3_io *io, struct switch_t1v3_result *result);

/* The one gate to the device: the allowlist decides, then the raw access runs. A refused access is not made and reads 0.
 * t1v3_gate.c; the raw accesses are in t1v3_access.c. */
uint32_t switch_t1v3_access(unsigned reg, int write, uint32_t value);

/* The loader's own io: t1v3_access.c, the only place a T1 v3 device address is used. */
void switch_t1v3_run_on_hardware(struct switch_t1v3_result *result);
#endif
