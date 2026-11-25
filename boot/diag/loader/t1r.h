#ifndef SWITCH_DIAG_T1R_H
#define SWITCH_DIAG_T1R_H
#include <stdint.h>

/* Every register T1-R may read, by name. There is no write: this stage has no store to a device address. */
enum switch_t1r_reg {
    T1R_CAR_RST_DEVICES_L,
    T1R_CAR_CLK_OUT_ENB_L,
    T1R_CAR_SOURCE_I2C1,
    T1R_CAR_SOURCE_I2C2,
    T1R_CAR_SOURCE_I2C3,
    T1R_CAR_SOURCE_I2C4,
    T1R_CAR_SOURCE_I2C5,
    T1R_CAR_SOURCE_I2C6,
    T1R_CAR_PLLP_BASE,
    T1R_CAR_PLLP_OUTA,
    T1R_CAR_PLLP_OUTB,
    T1R_I2C1_CNFG,
    T1R_I2C1_STATUS,
    T1R_I2C1_INT_STATUS,
    T1R_I2C1_CLK_DIVISOR,
    T1R_I2C1_CONFIG_LOAD,
    T1R_REG_COUNT
};

#define SWITCH_T1R_BUSES 6u

enum {
    SWITCH_T1R_DENIED = 0x601, /* the allowlist refused a read: a loader bug */
};

enum {
    SWITCH_T1R_I2C1_READ = 0,
    SWITCH_T1R_I2C1_CLOCK_OFF = 1,
    SWITCH_T1R_I2C1_IN_RESET = 2,
};

struct switch_t1r_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
};

struct switch_t1r_result {
    int stop;
    uint32_t rst_devices_l, clk_out_enb_l;
    uint32_t source[SWITCH_T1R_BUSES]; /* CLK_SOURCE_I2C1 to I2C6 */
    uint32_t pllp_base, pllp_outa, pllp_outb;
    int i2c1; /* SWITCH_T1R_I2C1_*: the five I2C1 words below are valid only for SWITCH_T1R_I2C1_READ */
    uint32_t cnfg, status, int_status, divisor, config_load;
};

/* Allowlist. The address of register reg, or 0 when reg is not a T1-R register. */
uint32_t switch_t1r_address(unsigned reg);
int switch_t1r_may_read(unsigned reg);

/* What hekate's clock_enable writes for I2C bus (0 for I2C1): (6 << 29) | divider field. */
uint32_t switch_t1r_hekate_source(unsigned bus);

/* Reads CAR first and reads no I2C1 register unless the clock is on and the block is out of reset. */
void switch_t1r_run(const struct switch_t1r_io *io, struct switch_t1r_result *result);

/* The one gate to the device: the allowlist decides, then the raw read runs. A refused read is not made and returns 0.
 * t1r_gate.c; the raw read is in t1r_access.c. */
uint32_t switch_t1r_access(unsigned reg);

/* t1r_access.c, the only place a T1-R device address is used. */
void switch_t1r_run_on_hardware(struct switch_t1r_result *result);
#endif
