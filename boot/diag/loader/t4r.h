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

/* Status bits 4 to 13 are mainline Linux's for WDT0 (tegra_wdt.c: WDT_STS_COUNT_SHIFT 4, mask 0xff, a countdown in quarter
 * periods; WDT_STS_EXP_SHIFT 12, mask 0x3, the expiry count). Bits 0 and 1 are only the heartbeat code's for WDT4 (enabled,
 * interrupt), never measured here, and WDT0 having the same layout is an assumption: the screen says so. Every other status
 * bit is undefined. PTV bit 31 enables the timer and bit 30 makes it periodic (Linux and hekate); the value is the bits below. */
#define SWITCH_T4R_PTV_ENABLE    (1u << 31)
#define SWITCH_T4R_PTV_VALUE     0x3fffffffu
#define SWITCH_T4R_STS_ENABLED   (1u << 0)
#define SWITCH_T4R_STS_INT       (1u << 1)
#define SWITCH_T4R_STS_COUNT(s)  (((s) >> 4) & 0xffu)
#define SWITCH_T4R_STS_EXPIRY(s) (((s) >> 12) & 0x3u)
#define SWITCH_T4R_STS_DEFINED   (0x3u | (0xffu << 4) | (0x3u << 12))
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
    T4R_STEP_STATE,
    T4R_STEP_FIELDS,
    T4R_STEP_TMRUS2_BEGIN,
    T4R_STEP_TMRUS2_RESULT,
    T4R_STEP_COMPLETE,
    T4R_STEP_COUNT
};

/* What the three WDT0 words amount to. Only what the sources give is classified; anything else is UNKNOWN with its reason. */
enum switch_t4r_state {
    T4R_STATE_NOT_CLASSIFIED = 0, /* a read did not happen */
    T4R_STATE_IDLE,               /* not enabled, timer stopped, config zero */
    T4R_STATE_CONFIGURED_STOPPED, /* not enabled, timer stopped, config not zero */
    T4R_STATE_DORMANT,            /* enabled on a stopped timer, source 5 */
    T4R_STATE_BUSY,               /* timer 5 enabled with a value, or the unconfirmed interrupt bit set */
    T4R_STATE_UNKNOWN,
};

enum switch_t4r_why {
    T4R_WHY_NONE = 0,
    T4R_WHY_ALL_ONES,       /* a word read as all ones: no register state, a bus answer */
    T4R_WHY_STATUS_BITS,    /* a status bit no source defines */
    T4R_WHY_TIMER_VALUE,    /* a stopped timer holding a value */
    T4R_WHY_OTHER_SOURCE,   /* enabled, but the config's source is not timer 5 */
    T4R_WHY_CONFLICT,       /* the timer is enabled with no value to count */
    T4R_WHY_COUNT_LEFT,     /* stopped, and the status still holds a count */
    T4R_WHY_EXPIRY_LEFT,    /* stopped, and the status still holds an expiry count */
    T4R_WHY_COUNT_AND_EXPIRY_LEFT,
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

/* The classification of the three words, a pure function of them. UNKNOWN, each with its reason: a word of all ones, a status
 * bit no source defines, an enabled timer with no value, a stopped timer with a value, a count or an expiry count left on a
 * stopped timer, a source other than timer 5. A running timer (PTV bit 31 with a value) is BUSY and its count and expiry
 * are shown. Nothing is stopped, set or cleared whatever the words are. */
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
