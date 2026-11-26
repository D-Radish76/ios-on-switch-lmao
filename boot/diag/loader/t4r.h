#ifndef SWITCH_DIAG_T4R_H
#define SWITCH_DIAG_T4R_H
#include <stdint.h>

/* The four registers T4-R may read, each as a 32 bit word, and nothing else: no store to a device address exists in this
 * stage, and it neither stops nor sets any watchdog. TMRUS is read twice. WDT0 is the TKE watchdog the panic path and
 * mainline Linux use (timer 5, config at 0x100); WDT4 and the other timers are not read. */
enum switch_t4r_reg {
    T4R_TMRUS,
    T4R_TMR5_PTV,
    T4R_WDT0_CONFIG,
    T4R_WDT0_STATUS,
    T4R_REG_COUNT
};

#define T4R_WIDTH 32u

/* Bits as the heartbeat code names them for WDT4's status and as Linux names the timer's PTV. That WDT0's status has the same
 * layout as WDT4's is an assumption, not a reading: the classification says so on the screen. */
#define SWITCH_T4R_PTV_ENABLE    (1u << 31)
#define SWITCH_T4R_STS_ENABLED   (1u << 0)
#define SWITCH_T4R_STS_INT       (1u << 1)
#define SWITCH_T4R_CFG_SOURCE(c) ((c)&0xfu)
#define SWITCH_T4R_WDT0_TIMER    5u
#define SWITCH_T4R_ALL_ONES      0xffffffffu

enum {
    SWITCH_T4R_DENIED = 0x901,    /* the allowlist refused a read: a loader bug */
    SWITCH_T4R_NOT_SHOWN = 0x902, /* a screen line was not drawn and settled, so the next read was not made */
};

/* One screen line per step. A BEGIN line is shown before its read, a RESULT line after it. */
enum switch_t4r_step {
    T4R_STEP_START,
    T4R_STEP_TMRUS1_BEGIN,
    T4R_STEP_TMRUS1_RESULT,
    T4R_STEP_PTV_BEGIN,
    T4R_STEP_PTV_RESULT,
    T4R_STEP_CONFIG_BEGIN,
    T4R_STEP_CONFIG_RESULT,
    T4R_STEP_STATUS_BEGIN,
    T4R_STEP_STATUS_RESULT,
    T4R_STEP_TMRUS2_BEGIN,
    T4R_STEP_TMRUS2_RESULT,
    T4R_STEP_STATE,
    T4R_STEP_COMPLETE,
    T4R_STEP_COUNT
};

/* What the three WDT0 words amount to. Only what the sources give is classified; anything else is UNKNOWN with its reason. */
enum switch_t4r_state {
    T4R_STATE_NOT_CLASSIFIED = 0, /* a read did not happen */
    T4R_STATE_IDLE,               /* not enabled, timer stopped, config zero */
    T4R_STATE_CONFIGURED_STOPPED, /* not enabled, timer stopped, config not zero */
    T4R_STATE_DORMANT,            /* enabled on a stopped timer, source 5 */
    T4R_STATE_BUSY,               /* timer 5 running, or an interrupt pending */
    T4R_STATE_UNKNOWN,
};

enum switch_t4r_why {
    T4R_WHY_NONE = 0,
    T4R_WHY_ALL_ONES,       /* a word read as all ones: no register state, a bus answer */
    T4R_WHY_STATUS_BITS,    /* status bits other than the two the heartbeat names */
    T4R_WHY_TIMER_VALUE,    /* a stopped timer holding a value */
    T4R_WHY_OTHER_SOURCE,   /* enabled, but the config's source is not timer 5 */
};

struct switch_t4r_result;

/* Draws the line of a step and returns nonzero once it is on the screen and settled. Zero stops the run before its next read. */
typedef int (*switch_t4r_show_fn)(void *context, unsigned step, const struct switch_t4r_result *result);

struct switch_t4r_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
    switch_t4r_show_fn show; /* null: no screen, every step counts as shown */
};

struct switch_t4r_result {
    int stop;
    unsigned reads;
    uint32_t tmrus_first, tmrus_second, ptv, config, status;
    int tmrus_advanced; /* set once both TMRUS reads were made and shown */
    unsigned state;     /* switch_t4r_state, set only when all three WDT0 words were read */
    unsigned why;       /* switch_t4r_why */
};

uint32_t switch_t4r_address(unsigned reg);
/* Nonzero only for a listed register at the one listed width, 32 bit. */
int switch_t4r_may_read(unsigned reg, unsigned width);

/* The classification of the three words, a pure function of them. A word of all ones, an unnamed status bit, a stopped timer
 * with a value and a source other than timer 5 are UNKNOWN: no source says what they mean. Nothing is stopped or set. */
unsigned switch_t4r_classify(uint32_t ptv, uint32_t config, uint32_t status, unsigned *why);
/* Nonzero when the second TMRUS read is later than the first by less than half the 32 bit range (the counter counts microseconds). */
int switch_t4r_advanced(uint32_t first, uint32_t second);

/* Reads TMRUS, TMR5_PTV, WDT0 config, WDT0 status, then TMRUS again. Every read is bracketed by shown steps: its BEGIN line
 * is on the screen before the read, its RESULT line before the next step. A read that hangs the bus is not left by this code,
 * and no software timeout can leave it: the person holds POWER. */
void switch_t4r_run(const struct switch_t4r_io *io, struct switch_t4r_result *result);

#define SWITCH_T4R_SETTLE_MS 100u
#define SWITCH_T4R_CNTFRQ    19200000u
uint64_t switch_t4r_settle_ticks(uint64_t cntfrq);

/* Barrier, then a wait of SWITCH_T4R_SETTLE_MS on the counter. Zero when the counter did not behave. t4r_settle.c. */
int switch_t4r_settle(void);

/* The one gate to the device: the allowlist decides, then the raw 32 bit read runs. A refused read is not made and returns 0. */
uint32_t switch_t4r_access(unsigned reg);

/* t4r_access.c, the only place a T4-R device address is used. */
void switch_t4r_run_on_hardware(struct switch_t4r_result *result, switch_t4r_show_fn show);
#endif
