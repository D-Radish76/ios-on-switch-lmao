#ifndef SWITCH_DIAG_T2_REGS_H
#define SWITCH_DIAG_T2_REGS_H
#include <stdint.h>

/* Every register the T2 image may touch, by name (ERISTA_T2_DESIGN.md). The CAR is read and never written. The names, the table
 * and the allowlist are T2's own: nothing here is shared with the earlier stages'. */
enum switch_t2_hw_reg {
    T2_HW_CAR_RST_DEVICES_L,
    T2_HW_CAR_CLK_OUT_ENB_L,
    T2_HW_CAR_CLK_SOURCE_I2C1,
    T2_HW_CAR_PLLP_BASE,
    T2_HW_I2C1_CNFG,
    T2_HW_I2C1_CMD_ADDR0,
    T2_HW_I2C1_CMD_DATA1,
    T2_HW_I2C1_STATUS,
    T2_HW_I2C1_INT_STATUS,
    T2_HW_I2C1_CLK_DIVISOR,
    T2_HW_I2C1_CONFIG_LOAD,
    T2_HW_I2C1_TIMING_0,
    T2_HW_I2C1_TIMING_1,
    T2_HW_REG_COUNT
};

/* What T1-T read on I2C1 (boot/diag/erista-t1t-run1.txt), and the divisor the earlier divisor test applied. */
#define SWITCH_T2_TIMING_0_PIN    0x00000204u
#define SWITCH_T2_TIMING_1_PIN    0x04070404u
#define SWITCH_T2_SOURCE_EXPECTED 0x00000003u
#define SWITCH_T2_DIVISOR_BEFORE  0x00050001u
#define SWITCH_T2_DIVISOR_AFTER   0x00ff0001u
#define SWITCH_T2_PLLP_M(word)    ((word) & 0xffu)
#define SWITCH_T2_PLLP_N(word)    (((word) >> 10) & 0xffu)
#define SWITCH_T2_PLLP_P(word)    (((word) >> 20) & 0x1fu)
#define SWITCH_T2_PLLP_M_EXPECTED 8u
#define SWITCH_T2_PLLP_N_EXPECTED 85u
#define SWITCH_T2_PLLP_P_EXPECTED 1u
#define SWITCH_T2_LOAD_REST       0x00000004u
#define SWITCH_T2_LOAD_APPLY      0x01u
#define SWITCH_T2_LOAD_TRANSFER   0x25u

#define SWITCH_T2_LOAD_TIMEOUT_US 1000u
#define SWITCH_T2_BUSY_TIMEOUT_US 100000u
#define SWITCH_T2_START_WINDOW_US 1000u
#define SWITCH_T2_IDLE_SAMPLES    3u
#define SWITCH_T2_IDLE_SPACING_US 100u
/* Every wait and poll loop also ends after this many iterations, whatever the counter says. */
#define SWITCH_T2_POLL_LIMIT      1000000u

/* Why the controller side stopped. The sensor sequence's codes are 0720 to 0725 (t2.h), the bus's 0730 to 0736 (t2_bus.h). */
enum {
    SWITCH_T2_I2C_CLOCK_OFF = 0x701,
    SWITCH_T2_I2C_IN_RESET = 0x702,
    SWITCH_T2_SOURCE_UNEXPECTED = 0x703,
    SWITCH_T2_BUS_BUSY = 0x704,
    SWITCH_T2_DIVISOR_UNEXPECTED = 0x705,
    SWITCH_T2_TIMING_ZERO = 0x706,
    SWITCH_T2_TIMING_UNEXPECTED = 0x707,
    SWITCH_T2_TIMING_UNREADABLE = 0x708,
    SWITCH_T2_IDLE_FAILED = 0x709,
    SWITCH_T2_CONFIG_LOAD_TIMEOUT = 0x70a,
    SWITCH_T2_DIVISOR_READBACK = 0x70b,
    SWITCH_T2_NOT_IDLE = 0x70f,
    SWITCH_T2_DENIED = 0x710,
    SWITCH_T2_NOT_ERISTA = 0x711,
    SWITCH_T2_COUNTER_STALLED = 0x712,
    SWITCH_T2_INT_PENDING = 0x713,
    SWITCH_T2_INT_CHANGED = 0x714,
    SWITCH_T2_LOAD_BEFORE_UNEXPECTED = 0x715,
    SWITCH_T2_LOAD_BEFORE_PENDING = 0x716,
    SWITCH_T2_LOAD_AFTER_UNEXPECTED = 0x717,
    SWITCH_T2_PLLP_UNEXPECTED = 0x71b,
};

struct switch_t2_io {
    void *context;
    uint32_t timing_0_pin, timing_1_pin;
    uint32_t (*read)(void *context, unsigned reg);
    void (*write)(void *context, unsigned reg, uint32_t value);
    uint64_t (*now_us)(void *context);
};

uint32_t switch_t2_address(unsigned reg);
int switch_t2_may_read(unsigned reg);
/* A write is allowed for one register and one of its listed values. */
int switch_t2_may_write(unsigned reg, uint32_t value);

/* The one gate to the device (t2_gate.c): the allowlist decides, then the raw access of t2_access.c runs. A refused access is
 * not made and reads 0. */
uint32_t switch_t2_access(unsigned reg, int write, uint32_t value);
#endif
