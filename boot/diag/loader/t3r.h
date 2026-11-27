#ifndef SWITCH_DIAG_T3R_H
#define SWITCH_DIAG_T3R_H
#include <stdint.h>

/* The 21 registers T3-R may read, each as a 32 bit word, and nothing else: no store to a device address exists in this
 * stage. It does not start the fan, switch the 5 V rail or change a pin. TMRUS is read twice, so a run makes 22 reads.
 * Addresses and bit meanings are hekate v6.5.5's (bdk/thermal/fan.c, bdk/power/regulator_5v.c, bdk/soc/gpio.c); see
 * ERISTA_T3R_DESIGN.md. */
enum switch_t3r_reg {
    T3R_TMRUS,
    T3R_CAR_RST_DEVICES_L,
    T3R_CAR_CLK_OUT_ENB_L,
    T3R_PWM_CSR_1,
    T3R_PINMUX_LCD_GPIO2,
    T3R_PINMUX_CAM1_PWDN,
    T3R_PINMUX_SATA_LED_ACTIVE,
    T3R_PINMUX_USB_VBUS_EN0,
    T3R_GPIO_A_CNF,
    T3R_GPIO_A_OE,
    T3R_GPIO_A_OUT,
    T3R_GPIO_CC_CNF,
    T3R_GPIO_CC_OE,
    T3R_GPIO_CC_OUT,
    T3R_GPIO_S_CNF,
    T3R_GPIO_S_OE,
    T3R_GPIO_S_IN,
    T3R_GPIO_V_CNF,
    T3R_GPIO_V_OE,
    T3R_PMC_NO_IOPOWER,
    T3R_PMC_PWR_DET_VAL,
    T3R_REG_COUNT
};

#define T3R_WIDTH      32u
#define T3R_READ_COUNT 22u

/* The register read at position index of a run, 0 to 21; T3R_REG_COUNT for any other index. */
unsigned switch_t3r_sequence(unsigned index);
uint32_t switch_t3r_address(unsigned reg);
/* Nonzero only for a listed register at the one listed width, 32 bit. */
int switch_t3r_may_read(unsigned reg, unsigned width);

/* PWM_CSR_1 as hekate writes it: bit 31 enables, bit 24 means absolute 0 percent, bits 23:16 hold the inverted duty
 * (236 minus the duty), the rest is zero. The off word is 0x81000000. A field above 236 or a field of 236 is a value
 * hekate never writes. */
#define SWITCH_T3R_PWM_EN          (1u << 31)
#define SWITCH_T3R_PWM_ABSOLUTE_0  (1u << 24)
#define SWITCH_T3R_PWM_OFF_WORD    0x81000000u
#define SWITCH_T3R_DUTY_MAX        236u
#define SWITCH_T3R_CLOCK_PWM_BIT   (1u << 17)
#define SWITCH_T3R_ALL_ONES        0xffffffffu

enum {
    SWITCH_T3R_DENIED = 0x901,    /* the allowlist refused a read: a loader bug */
    SWITCH_T3R_NOT_SHOWN = 0x902, /* a screen line was not drawn and settled, so the next read was not made */
    SWITCH_T3R_TIME = 0x903,      /* the read budget ran out before the next read */
    SWITCH_T3R_STOP_ALL_ONES = 0x904,  /* a word read as all ones: a bus answer, not register state */
};

enum switch_t3r_gate {
    T3R_GATE_UNDECIDED = 0,
    T3R_GATE_OPEN,
    T3R_GATE_CLOCK_OFF,
    T3R_GATE_IN_RESET,
};

enum switch_t3r_pwm {
    T3R_PWM_NOT_CLASSIFIED = 0, /* the word was not read */
    T3R_PWM_DISABLED,           /* the word is zero: the enable bit is clear and nothing else is set */
    T3R_PWM_OFF_HEKATE,         /* enable and absolute 0 percent, nothing else: hekate's off word */
    T3R_PWM_DUTY,               /* enable, field 0 to 235: a duty hekate can write */
    T3R_PWM_UNKNOWN,
};

enum switch_t3r_pwm_why {
    T3R_PWM_WHY_NONE = 0,
    T3R_PWM_WHY_ALL_ONES,
    T3R_PWM_WHY_ENABLE_CLEAR_BITS,   /* the enable bit is clear and other bits are set */
    T3R_PWM_WHY_ABSOLUTE_0_BITS,     /* absolute 0 percent together with another bit */
    T3R_PWM_WHY_OTHER_BITS,          /* bits outside the enable bit, bit 24 and the field */
    T3R_PWM_WHY_SPECIAL_ZERO_DUTY,   /* field 236: duty 0 by the formula, which hekate writes as the off word instead */
    T3R_PWM_WHY_OUT_OF_RANGE,        /* field 237 to 255 */
};

/* Comparison with what hekate's code leaves after a Nyx exit (fan_set_duty(0) from hw_deinit). Information, not a verdict. */
enum switch_t3r_expect {
    T3R_EXPECT_NONE = 0,
    T3R_EXPECT_MATCH,
    T3R_EXPECT_DIFFERS,
};

enum switch_t3r_step {
    T3R_STEP_START = 0,
    /* BEGIN of read i is 1 + 2 i, its RESULT 2 + 2 i, for i = 0 to 21 */
    T3R_STEP_SKIP_PWM = 1 + 2 * T3R_READ_COUNT,
    T3R_STEP_CAR,
    T3R_STEP_FAN,
    T3R_STEP_ROUTE,
    T3R_STEP_RAIL,
    T3R_STEP_TACH,
    T3R_STEP_COMPLETE,
    T3R_STEP_COUNT
};
#define SWITCH_T3R_STEP_BEGIN(i)  (1u + 2u * (i))
#define SWITCH_T3R_STEP_RESULT(i) (2u + 2u * (i))

struct switch_t3r_result;

/* Draws the line of a step and returns nonzero once it is on the screen and settled. Zero stops the run before its next read. */
typedef int (*switch_t3r_show_fn)(void *context, unsigned step, const struct switch_t3r_result *result);

struct switch_t3r_io {
    void *context;
    uint32_t (*read)(void *context, unsigned reg);
    switch_t3r_show_fn show; /* null: no screen, every step counts as shown */
    int (*expired)(void *context); /* null: no budget; nonzero ends the run before the next read */
};

struct switch_t3r_result {
    int stop;
    unsigned reads;
    uint32_t value[T3R_REG_COUNT];
    uint32_t seen; /* bit r set once register r was read */
    uint32_t tmrus_second;
    int tmrus_advanced;
    unsigned gate;    /* switch_t3r_gate */
    unsigned pwm;     /* switch_t3r_pwm */
    unsigned pwm_why; /* switch_t3r_pwm_why */
    unsigned pwm_duty;
};

/* The PWM clock and reset words of CAR: open only when bit 17 of CLK_OUT_ENB_L is set and bit 17 of RST_DEVICES_L is clear. */
unsigned switch_t3r_pwm_gate(uint32_t rst_devices_l, uint32_t clk_out_enb_l);
/* A pure function of the PWM_CSR_1 word. Nothing is written whatever the word is. */
unsigned switch_t3r_pwm_classify(uint32_t word, unsigned *why, unsigned *duty);
unsigned switch_t3r_expect(unsigned reg, uint32_t value);
/* Nonzero when the second TMRUS read is later than the first by less than half the 32 bit range (the counter counts microseconds). */
int switch_t3r_advanced(uint32_t first, uint32_t second);

/* Runs the 22 reads in order. Every read is bracketed by shown steps: its BEGIN line is on the screen before the read, its
 * RESULT line before the next step. The PWM read is made only when the gate is open. A read that hangs the bus is not left by
 * this code, and no software timeout can leave it: the person holds POWER. */
void switch_t3r_run(const struct switch_t3r_io *io, struct switch_t3r_result *result);

#define SWITCH_T3R_SETTLE_MS             100u
#define SWITCH_T3R_CNTFRQ                19200000u
#define SWITCH_T3R_READ_BUDGET_SECONDS   20u
uint64_t switch_t3r_settle_ticks(uint64_t cntfrq);
uint64_t switch_t3r_budget_ticks(uint64_t cntfrq);

/* Barrier, then a wait of SWITCH_T3R_SETTLE_MS on the counter. Zero when the counter did not behave. t3r_settle.c. */
int switch_t3r_settle(void);
/* The counter reading, and whether SWITCH_T3R_READ_BUDGET_SECONDS have passed since start. t3r_settle.c. */
uint64_t switch_t3r_counter(void);
int switch_t3r_budget_spent(uint64_t start);

/* The one gate to the device: the allowlist decides, then the raw 32 bit read runs. A refused read is not made and returns 0. */
uint32_t switch_t3r_access(unsigned reg);

/* t3r_access.c, the only place a T3-R device address is used. */
void switch_t3r_run_on_hardware(struct switch_t3r_result *result, switch_t3r_show_fn show);
#endif
