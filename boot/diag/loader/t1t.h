#ifndef SWITCH_DIAG_T1T_H
#define SWITCH_DIAG_T1T_H
#include <stdint.h>

/* Every register T1-T may read, by name. There is no write: this stage has no store to a device address. */
enum switch_t1t_reg {
    T1T_CAR_RST_DEVICES_L,
    T1T_CAR_RST_DEVICES_H,
    T1T_CAR_CLK_OUT_ENB_L,
    T1T_CAR_CLK_OUT_ENB_H,
    T1T_CAR_SOURCE_I2C1,
    T1T_CAR_SOURCE_I2C2,
    T1T_CAR_SOURCE_I2C3,
    T1T_CAR_SOURCE_I2C4,
    T1T_CAR_SOURCE_I2C5,
    T1T_CAR_SOURCE_I2C6,
    T1T_CAR_PLLP_BASE,
    T1T_I2C_FIRST,
    T1T_REG_COUNT = T1T_I2C_FIRST + 14
};

/* The words read from one controller, in the order of the allowlist: block 0 is I2C1, block 1 is I2C5. */
enum {
    T1T_CNFG,
    T1T_STATUS,
    T1T_INT_STATUS,
    T1T_CLK_DIVISOR,
    T1T_CONFIG_LOAD,
    T1T_TIMING_0,
    T1T_TIMING_1,
    T1T_BLOCK_WORDS
};
#define SWITCH_T1T_BLOCKS 2u
#define SWITCH_T1T_BUSES  6u
#define SWITCH_T1T_I2C_REG(block, word) (T1T_I2C_FIRST + (block) * T1T_BLOCK_WORDS + (word))

enum {
    SWITCH_T1T_DENIED = 0x701, /* the allowlist refused a read: a loader bug */
};

enum {
    SWITCH_T1T_BLOCK_READ = 0,
    SWITCH_T1T_BLOCK_CLOCK_OFF = 1,
    SWITCH_T1T_BLOCK_IN_RESET = 2,
};

/* Fields as Linux names them. Reserved bits are only the bits outside every field mask. */
#define SWITCH_T1T_TLOW(word)    ((word) & 0x3fu)
#define SWITCH_T1T_THIGH(word)   (((word) >> 8) & 0x3fu)
#define SWITCH_T1T_TSU_STA(word) ((word) & 0x3fu)
#define SWITCH_T1T_THD_STA(word) (((word) >> 8) & 0x3fu)
#define SWITCH_T1T_TSU_STO(word) (((word) >> 16) & 0x3fu)
#define SWITCH_T1T_TBUF(word)    (((word) >> 24) & 0x3fu)
#define SWITCH_T1T_TIMING_0_FIELDS 0x00003f3fu
#define SWITCH_T1T_TIMING_1_FIELDS 0x3f3f3f3fu
/* T210 PLLP_BASE fields: M bits 7:0, N bits 17:10, P bits 24:20. */
#define SWITCH_T1T_PLLP_M(word) ((word) & 0xffu)
#define SWITCH_T1T_PLLP_N(word) (((word) >> 10) & 0xffu)
#define SWITCH_T1T_PLLP_P(word) (((word) >> 20) & 0x1fu)

struct switch_t1t_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
};

struct switch_t1t_block {
    int state; /* SWITCH_T1T_BLOCK_*: the words are valid only for SWITCH_T1T_BLOCK_READ */
    uint32_t word[T1T_BLOCK_WORDS];
};

struct switch_t1t_result {
    int stop;
    unsigned reads;
    uint32_t rst_devices_l, rst_devices_h, clk_out_enb_l, clk_out_enb_h;
    uint32_t source[SWITCH_T1T_BUSES]; /* CLK_SOURCE_I2C1 to I2C6 */
    uint32_t pllp_base;
    struct switch_t1t_block block[SWITCH_T1T_BLOCKS];
};

/* Allowlist. The address of register reg, or 0 when reg is not a T1-T register. */
uint32_t switch_t1t_address(unsigned reg);
int switch_t1t_may_read(unsigned reg);

/* Reads the CAR first, and the registers of a controller only when its clock is on and it is out of reset. */
void switch_t1t_run(const struct switch_t1t_io *io, struct switch_t1t_result *result);

/* The one gate to the device: the allowlist decides, then the raw read runs. A refused read is not made and returns 0.
 * t1t_gate.c; the raw read is in t1t_access.c. */
uint32_t switch_t1t_access(unsigned reg);

/* t1t_access.c, the only place a T1-T device address is used. */
void switch_t1t_run_on_hardware(struct switch_t1t_result *result);
#endif
