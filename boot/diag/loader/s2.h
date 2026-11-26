#ifndef SWITCH_DIAG_S2_H
#define SWITCH_DIAG_S2_H
#include <stdint.h>

/* Every register D0-S v2 may read, by name. There is no write: this stage has no store to a device address. */
enum switch_s2_reg {
    S2_CAR_RST_DEVICES_L,
    S2_CAR_CLK_OUT_ENB_L,
    S2_SDMMC1_PRESENT_STATE,
    S2_SDMMC1_WORD_2C,
    S2_REG_COUNT
};

#define SWITCH_S2_SDMMC1_CAR_BIT (1u << 14)

enum {
    SWITCH_S2_DENIED = 0x801, /* the allowlist refused a read: a loader bug */
};

enum {
    SWITCH_S2_SD_READ = 0,
    SWITCH_S2_SD_CLOCK_OFF = 1,
    SWITCH_S2_SD_IN_RESET = 2,
};

struct switch_s2_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
};

struct switch_s2_result {
    int stop;
    unsigned reads;
    uint32_t rst_devices_l, clk_out_enb_l;
    int sd_state; /* SWITCH_S2_SD_*: the two words below are valid only for SWITCH_S2_SD_READ */
    uint32_t sd_present_state, sd_word_2c;
};

uint32_t switch_s2_address(unsigned reg);
int switch_s2_may_read(unsigned reg);

/* Reads the two CAR words first, and the SDMMC1 words only when its clock is on and it is out of reset. */
void switch_s2_run(const struct switch_s2_io *io, struct switch_s2_result *result);
#endif
