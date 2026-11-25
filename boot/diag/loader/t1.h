#ifndef SWITCH_DIAG_T1_H
#define SWITCH_DIAG_T1_H
#include <stdint.h>

/* Every register T1 may touch, by name. Nothing else in the image forms a device address for T1. */
enum switch_t1_reg {
    T1_CAR_RST_DEVICES_L,
    T1_CAR_CLK_OUT_ENB_L,
    T1_CAR_CLK_SOURCE_PWM,
    T1_CAR_CLK_SOURCE_I2C1,
    T1_PINMUX_I2C1_SCL,
    T1_PINMUX_I2C1_SDA,
    T1_I2C1_CNFG,
    T1_I2C1_CMD_ADDR0,
    T1_I2C1_CMD_DATA1,
    T1_I2C1_STATUS,
    T1_I2C1_INT_STATUS,
    T1_I2C1_CLK_DIVISOR,
    T1_I2C1_CONFIG_LOAD,
    T1_REG_COUNT
};

#define SWITCH_T1_TMP451_ADDRESS 0x4cu
#define SWITCH_T1_DIVISOR_HEKATE 0x00050001u
#define SWITCH_T1_BUSY_TIMEOUT_US   100000u
#define SWITCH_T1_LOAD_TIMEOUT_US   1000u
#define SWITCH_T1_REGISTERS         5u

/* Why T1 stopped. 0 is a completed run; any other code ends the I2C work at once, with no retry. */
enum {
    SWITCH_T1_I2C_CLOCK_OFF = 0x501,
    SWITCH_T1_I2C_IN_RESET = 0x502,
    SWITCH_T1_BUS_BUSY = 0x503,
    SWITCH_T1_DIVISOR_UNEXPECTED = 0x504,
    SWITCH_T1_CONFIG_LOAD_TIMEOUT = 0x505,
    SWITCH_T1_BUSY_TIMEOUT = 0x506,
    SWITCH_T1_NOACK = 0x507,
    SWITCH_T1_ID_MISMATCH = 0x508,
    SWITCH_T1_NOT_IDLE = 0x509,
    SWITCH_T1_DENIED = 0x50a,
    SWITCH_T1_NOT_ERISTA = 0x50b,
};

/* The TMP451 registers T1 reads, in the order it reads them. Status (0x02) is not among them: reading it clears the
 * latched flags T2 needs to see. */
extern const uint8_t switch_t1_tmp451_register[SWITCH_T1_REGISTERS];

struct switch_t1_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
    void (*write)(void *context, unsigned reg, uint32_t value);
    uint64_t (*now_us)(void *context);
};

struct switch_t1_result {
    int stop;
    unsigned transfers;
    uint32_t rst_devices_l, clk_out_enb_l, source_i2c1, source_pwm;
    uint32_t pad_scl, pad_sda;
    uint32_t cnfg, status, int_status, divisor, config_load;
    uint32_t status_after, int_status_after;
    uint8_t value[SWITCH_T1_REGISTERS];
    unsigned have; /* bit i: value[i] was read */
};

/* Allowlist. The address of register reg, or 0 when reg is not a T1 register. */
uint32_t switch_t1_address(unsigned reg);
int switch_t1_may_read(unsigned reg);
/* A write is allowed for one register and one of its listed values. */
int switch_t1_may_write(unsigned reg, uint32_t value);

int switch_t1_i2c1_clocked(const struct switch_t1_result *result);
int switch_t1_pwm_clocked(const struct switch_t1_result *result);

/* The whole check. Reads CAR first and touches no I2C1 register unless the clock is on and the block is out of reset. */
void switch_t1_run(const struct switch_t1_io *io, struct switch_t1_result *result);

/* The one gate to the device: the allowlist decides, then the raw access runs. A refused access is not made and reads 0.
 * t1_gate.c; the raw accesses are in t1_access.c. */
uint32_t switch_t1_access(unsigned reg, int write, uint32_t value);

/* The loader's own io: t1_access.c, the only place a T1 device address is used. */
void switch_t1_run_on_hardware(struct switch_t1_result *result);
#endif
