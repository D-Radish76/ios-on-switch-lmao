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
    SWITCH_S2_DENIED = 0x801,      /* the allowlist refused a read: a loader bug */
    SWITCH_S2_NOT_SHOWN = 0x802,   /* a screen line was not drawn and settled, so the next read was not made */
};

/* One screen line per step, in this order. A BEGIN line is shown before its read, a RESULT line after it. */
enum switch_s2_step {
    S2_STEP_START,
    S2_STEP_CAR_RST_BEGIN,
    S2_STEP_CAR_RST_RESULT,
    S2_STEP_CAR_CLK_BEGIN,
    S2_STEP_CAR_CLK_RESULT,
    S2_STEP_GATE,
    S2_STEP_PRESENT_BEGIN,
    S2_STEP_PRESENT_RESULT,
    S2_STEP_CLOCK_WORD_BEGIN,
    S2_STEP_CLOCK_WORD_RESULT,
    S2_STEP_COMPLETE,
    S2_STEP_COUNT
};

enum {
    SWITCH_S2_SD_READ = 0,
    SWITCH_S2_SD_CLOCK_OFF = 1,
    SWITCH_S2_SD_IN_RESET = 2,
};

struct switch_s2_result;

/* Draws the line of a step and returns nonzero once it is on the screen and settled. Zero stops the run before its next read. */
typedef int (*switch_s2_show_fn)(void *context, unsigned step, const struct switch_s2_result *result);

struct switch_s2_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
    switch_s2_show_fn show; /* null: no screen, every step counts as shown */
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

/* Reads the two CAR words first, and the SDMMC1 words only when its clock is on and it is out of reset. Every read is
 * bracketed by shown steps: its BEGIN line is on the screen before the read, its RESULT line before the next step. */
void switch_s2_run(const struct switch_s2_io *io, struct switch_s2_result *result);

/* Counter ticks one settle lasts, or 0 when the frequency is not the 19.2 MHz this SoC's counter runs at. */
#define SWITCH_S2_SETTLE_MS 100u
#define SWITCH_S2_CNTFRQ    19200000u
uint64_t switch_s2_settle_ticks(uint64_t cntfrq);

/* Barrier, then a wait of SWITCH_S2_SETTLE_MS on the counter, long enough for several display frames. Nonzero when the wait
 * ran; zero when the counter did not behave, and nothing may be read on the strength of the screen. s2_settle.c. */
int switch_s2_settle(void);

/* The one gate to the device: the allowlist decides, then the raw read runs. s2_gate.c; the raw read is in s2_access.c. */
uint32_t switch_s2_access(unsigned reg);

/* s2_access.c, the only place an S2 device address is used. */
void switch_s2_run_on_hardware(struct switch_s2_result *result, switch_s2_show_fn show);
#endif
