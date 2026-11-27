#ifndef SWITCH_DIAG_T3_H
#define SWITCH_DIAG_T3_H
#include <stdint.h>

/* T3 turns the fan on at duty 51 for a few seconds and counts tachometer transitions. It reads 20 registers as 32 bit words
 * and makes six reviewed writes, each of one exact value; the allowlist is switch_t3_may_read and switch_t3_may_write. Addresses
 * and values are hekate v6.5.5's (bdk/thermal/fan.c, bdk/power/regulator_5v.c); see ERISTA_T3_FAN_DESIGN.md. */
enum switch_t3_reg {
    T3_CAR_RST_DEVICES_L,
    T3_CAR_CLK_OUT_ENB_L,
    T3_PWM_CSR_1,
    T3_PINMUX_LCD_GPIO2,
    T3_PINMUX_CAM1_PWDN,
    T3_PINMUX_SATA_LED_ACTIVE,
    T3_PINMUX_USB_VBUS_EN0,
    T3_GPIO_A_CNF,
    T3_GPIO_A_OE,
    T3_GPIO_A_OUT,
    T3_GPIO_CC_CNF,
    T3_GPIO_CC_OE,
    T3_GPIO_CC_OUT,
    T3_GPIO_S_CNF,
    T3_GPIO_S_OE,
    T3_GPIO_S_IN,
    T3_GPIO_V_CNF,
    T3_GPIO_V_OE,
    T3_PMC_NO_IOPOWER,
    T3_PMC_PWR_DET_VAL,
    T3_REG_COUNT
};

#define T3_WIDTH 32u

/* The words written, and the only values the gate lets through. The rail word restores GPIO A's OUT register to the zero the
 * state check requires before the first write, so no read-modify-write is needed. */
#define SWITCH_T3_RAIL_ON_WORD   0x00000020u /* GPIO_A_OUT: A5 high */
#define SWITCH_T3_RAIL_OFF_WORD  0x00000000u
#define SWITCH_T3_PWM_DUTY51     0x80b90000u /* PWM_CSR_1: enable, 236 - 51 = 185 = 0xb9 in bits 23:16 */
#define SWITCH_T3_PWM_OFF_WORD   0x81000000u
#define SWITCH_T3_ROUTE_PWM_WORD 0x00000001u /* LCD_GPIO2: function PWM1, no tristate, no pull */
#define SWITCH_T3_PARK_PAD_WORD  0x00000074u /* LCD_GPIO2 as found: parked, tristate, pull down */

enum switch_t3_write {
    T3_W_RAIL_ON,
    T3_W_PWM_DUTY,
    T3_W_ROUTE_PWM,
    T3_O_PWM_OFF,
    T3_O_RAIL_OFF,
    T3_O_PARK_PAD,
    T3_WRITE_COUNT
};

#define SWITCH_T3_CAR_PWM_BIT   (1u << 17)
#define SWITCH_T3_PWR_DET_GPIO_BIT 21u
#define SWITCH_T3_ALL_ONES      0xffffffffu

/* The state the first write needs, one reason per register group. The first failing one is reported. */
enum switch_t3_state {
    T3_STATE_OK = 0,
    T3_STATE_GATE,
    T3_STATE_PWM,
    T3_STATE_LCD_PAD,
    T3_STATE_TACH_PAD,
    T3_STATE_RAIL_PIN,
    T3_STATE_USB_PIN,
    T3_STATE_A5,
    T3_STATE_CC4,
    T3_STATE_S7,
    T3_STATE_V4,
    T3_STATE_PMC
};

enum {
    SWITCH_T3_DENIED = 0xa01,   /* the allowlist refused an access: a loader bug */
    SWITCH_T3_NOT_SHOWN = 0xa02, /* a screen line was not drawn and settled */
    SWITCH_T3_TIME = 0xa03,     /* the state reads ran past their budget */
    SWITCH_T3_STOP_ALL_ONES = 0xa04, /* a word read as all ones: a bus answer, not register state */
    SWITCH_T3_STATE = 0xa05,    /* the state differs from the one the writes assume: nothing was written */
    SWITCH_T3_READBACK = 0xa06, /* a write did not read back as written */
    SWITCH_T3_RAIL_TIME = 0xa07,
    SWITCH_T3_NO_EDGES = 0xa08, /* the spin window did not show more transitions than the two before it */
    SWITCH_T3_SAMPLES = 0xa09,  /* a window held too few samples to say anything */
    SWITCH_T3_WINDOW = 0xa0a,   /* a window ended by its sample limit, not by its clock */
};

/* Phase p is announced by step 1 + 2 p and reported by 2 + 2 p. */
enum switch_t3_phase {
    T3_PHASE_STATE,
    T3_PHASE_IDLE,
    T3_PHASE_RAIL,
    T3_PHASE_SPIN,
    T3_PHASE_OFF,
    T3_PHASE_STOPPED,
    T3_PHASE_COUNT
};
#define SWITCH_T3_STEP_START       0u
#define SWITCH_T3_STEP_BEGIN(p)    (1u + 2u * (p))
#define SWITCH_T3_STEP_DONE(p)     (2u + 2u * (p))
#define SWITCH_T3_STEP_COMPLETE    (1u + 2u * T3_PHASE_COUNT)

/* Tachometer windows. Spin-up is counted too, so the screen shows it. */
enum switch_t3_window_id {
    T3_WINDOW_IDLE,
    T3_WINDOW_RAIL,
    T3_WINDOW_SPINUP,
    T3_WINDOW_SPIN,
    T3_WINDOW_STOPPED,
    T3_WINDOW_COUNT
};

enum switch_t3_cut {
    T3_CUT_NONE = 0, /* ended by its clock */
    T3_CUT_RAIL,     /* the rail time limit ended it */
    T3_CUT_LIMIT,    /* the sample limit ended it */
};

struct switch_t3_window {
    int ran;
    unsigned cut;
    uint32_t samples;
    uint32_t high;
    uint32_t transitions;
    unsigned first_level;
    unsigned last_level;
};

enum switch_t3_verdict {
    T3_VERDICT_NONE = 0,
    T3_VERDICT_PASS,
    T3_VERDICT_NO_EDGES,
    T3_VERDICT_FEW_SAMPLES,
};

#define SWITCH_T3_TACH_BIT          7u
#define SWITCH_T3_MIN_SAMPLES       1000u
#define SWITCH_T3_MIN_TRANSITIONS   4u
#define SWITCH_T3_SAMPLE_LIMIT      100000000u
#define SWITCH_T3_WINDOW_MS         2000u
#define SWITCH_T3_SPINUP_MS         1000u
#define SWITCH_T3_RAIL_LIMIT_MS     15000u
#define SWITCH_T3_READ_BUDGET_MS    20000u
#define SWITCH_T3_SETTLE_MS         100u
#define SWITCH_T3_CNTFRQ            19200000u

struct switch_t3_result;
typedef int (*switch_t3_show_fn)(void *context, unsigned step, const struct switch_t3_result *result);

struct switch_t3_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
    void (*write)(void *context, unsigned reg, uint32_t value);
    switch_t3_show_fn show;              /* null: no screen, every step counts as shown */
    int (*read_expired)(void *context);  /* nonzero ends the state reads */
    void (*rail_mark)(void *context);    /* the rail time limit counts from this call */
    int (*rail_expired)(void *context);  /* nonzero once the rail has been on for SWITCH_T3_RAIL_LIMIT_MS; nonzero if no clock */
    void (*window_begin)(void *context, unsigned milliseconds);
    int (*window_over)(void *context);   /* nonzero once the window's time has passed; nonzero if no clock */
};

struct switch_t3_result {
    int stop;
    unsigned reads;
    unsigned writes;
    uint32_t value[T3_REG_COUNT];
    unsigned state;
    unsigned baseline_level; /* tach pad level in the state read, whatever it is */
    uint32_t issued;         /* bit w set once write w was sent to the gate */
    uint32_t mismatched;     /* bit w set when write w did not read back as written */
    uint32_t readback[T3_WRITE_COUNT];
    int rail_on;
    struct switch_t3_window window[T3_WINDOW_COUNT];
    unsigned verdict;
};

uint32_t switch_t3_address(unsigned reg);
int switch_t3_may_read(unsigned reg, unsigned width);
/* Nonzero only for one of the six listed words at the register it is listed for. */
int switch_t3_may_write(unsigned reg, uint32_t value);
unsigned switch_t3_write_reg(unsigned write);
uint32_t switch_t3_write_value(unsigned write);

/* A pure function of the state words: T3_STATE_OK or the first group that differs. */
unsigned switch_t3_state_check(const uint32_t value[T3_REG_COUNT]);
unsigned switch_t3_verdict(const struct switch_t3_window window[T3_WINDOW_COUNT]);
/* Counts changes of the tach pad level for the window's time; the level it starts at is not fixed. */
void switch_t3_count_window(const struct switch_t3_io *io, struct switch_t3_result *result, unsigned id, unsigned milliseconds);

/* Runs the state reads, the five windows and the off sequence. The off sequence undoes every write that was sent, in the
 * order PWM, rail, pad, whatever stopped the run, and goes on after a readback that differs. A hang of the bus is not left by
 * this code: the person holds POWER. */
void switch_t3_run(const struct switch_t3_io *io, struct switch_t3_result *result);

uint64_t switch_t3_ms_ticks(uint64_t cntfrq, unsigned milliseconds);

/* t3_settle.c */
uint64_t switch_t3_counter(void);
int switch_t3_settle(void);
/* Nonzero once milliseconds have passed since start on the counter, or when the counter is not the expected 19.2 MHz one. */
int switch_t3_spent(uint64_t start, unsigned milliseconds);

/* The one gate to the device: the allowlist decides, then the raw 32 bit access runs. A refused access is not made. */
uint32_t switch_t3_access(unsigned reg, int write, uint32_t value);

/* t3_access.c, the only place a T3 device address is used. */
void switch_t3_run_on_hardware(struct switch_t3_result *result, switch_t3_show_fn show);
#endif
