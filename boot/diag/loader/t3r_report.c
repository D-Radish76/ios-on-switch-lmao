#include "t3r_report.h"

static const char *const names[T3R_REG_COUNT] = {
    [T3R_TMRUS] = "TMRUS",
    [T3R_CAR_RST_DEVICES_L] = "RST_DEV_L",
    [T3R_CAR_CLK_OUT_ENB_L] = "CLK_ENB_L",
    [T3R_PWM_CSR_1] = "PWM_CSR_1",
    [T3R_PINMUX_LCD_GPIO2] = "LCD_GPIO2",
    [T3R_PINMUX_CAM1_PWDN] = "CAM1_PWDN",
    [T3R_PINMUX_SATA_LED_ACTIVE] = "SATA_LED",
    [T3R_PINMUX_USB_VBUS_EN0] = "USB_VBUS0",
    [T3R_GPIO_A_CNF] = "GPIO_A_CNF",
    [T3R_GPIO_A_OE] = "GPIO_A_OE",
    [T3R_GPIO_A_OUT] = "GPIO_A_OUT",
    [T3R_GPIO_CC_CNF] = "GPIO_CC_CNF",
    [T3R_GPIO_CC_OE] = "GPIO_CC_OE",
    [T3R_GPIO_CC_OUT] = "GPIO_CC_OUT",
    [T3R_GPIO_S_CNF] = "GPIO_S_CNF",
    [T3R_GPIO_S_OE] = "GPIO_S_OE",
    [T3R_GPIO_S_IN] = "GPIO_S_IN",
    [T3R_GPIO_V_CNF] = "GPIO_V_CNF",
    [T3R_GPIO_V_OE] = "GPIO_V_OE",
    [T3R_PMC_NO_IOPOWER] = "PMC_NOIOPWR",
    [T3R_PMC_PWR_DET_VAL] = "PMC_PWRDET",
};

static void pad(struct switch_t3r_line *out)
{
    while (out->text.length < SWITCH_T3R_CELL)
        switch_diag_line_add(&out->text, " ");
}

static void place(struct switch_t3r_line *out, unsigned index)
{
    out->row = index % SWITCH_T3R_COLUMN_ROWS;
    out->right = index >= SWITCH_T3R_COLUMN_ROWS;
}

static void head(struct switch_t3r_line *out, unsigned index, unsigned reg)
{
    place(out, index);
    switch_diag_line_start(&out->text, "");
    if (index + 1 < 10)
        switch_diag_line_add(&out->text, "0");
    switch_diag_line_decimal(&out->text, index + 1);
    switch_diag_line_add(&out->text, " ");
    switch_diag_line_add(&out->text, names[reg]);
    switch_diag_line_add(&out->text, " ");
    switch_diag_line_hex(&out->text, switch_t3r_address(reg), 8);
}

static const char *pwm_text(unsigned pwm)
{
    switch (pwm) {
    case T3R_PWM_DISABLED: return " DISABLED";
    case T3R_PWM_OFF_HEKATE: return " OFF AS HEKATE";
    case T3R_PWM_DUTY: return " DUTY";
    case T3R_PWM_UNKNOWN: return " UNKNOWN";
    default: return "";
    }
}

static void tag(struct switch_t3r_line *out, const struct switch_t3r_result *r, unsigned reg, uint32_t value)
{
    switch (reg) {
    case T3R_CAR_RST_DEVICES_L:
        switch_diag_line_add(&out->text, value & SWITCH_T3R_CLOCK_PWM_BIT ? " PWM IN RESET" : " PWM OUT OF RESET");
        out->bad = value == SWITCH_T3R_ALL_ONES;
        return;
    case T3R_CAR_CLK_OUT_ENB_L:
        switch_diag_line_add(&out->text, value & SWITCH_T3R_CLOCK_PWM_BIT ? " PWM CLOCK ON" : " PWM CLOCK OFF");
        out->bad = r->gate != T3R_GATE_OPEN;
        return;
    case T3R_PWM_CSR_1:
        switch_diag_line_add(&out->text, pwm_text(r->pwm));
        out->bad = r->pwm == T3R_PWM_UNKNOWN;
        return;
    default: break;
    }
    const unsigned expect = switch_t3r_expect(reg, value);
    if (expect != T3R_EXPECT_NONE)
        switch_diag_line_add(&out->text, expect == T3R_EXPECT_MATCH ? " AS EXPECTED" : " DIFFERS");
    if (value == SWITCH_T3R_ALL_ONES && reg != T3R_TMRUS) {
        switch_diag_line_add(&out->text, " ALL ONES. STOP");
        out->bad = 1;
    }
}

static void result_line(struct switch_t3r_line *out, const struct switch_t3r_result *r, unsigned index, unsigned reg)
{
    const int second = index == T3R_READ_COUNT - 1;
    const uint32_t value = second ? r->tmrus_second : r->value[reg];
    head(out, index, reg);
    switch_diag_line_add(&out->text, " = ");
    switch_diag_line_hex(&out->text, value, 8);
    if (second) {
        if (r->tmrus_advanced) {
            switch_diag_line_add(&out->text, " ADV ");
            switch_diag_line_decimal(&out->text, (uint32_t)(r->tmrus_second - r->value[T3R_TMRUS]));
            switch_diag_line_add(&out->text, " US");
        } else {
            switch_diag_line_add(&out->text, " DID NOT ADVANCE");
            out->bad = 1;
        }
    } else {
        tag(out, r, reg, value);
    }
    pad(out);
}

static void bit_text(struct switch_diag_line *line, const char *label, uint32_t word, unsigned bit)
{
    switch_diag_line_add(line, label);
    switch_diag_line_add(line, word >> bit & 1u ? "1" : "0");
}

static const char *why_text(unsigned why)
{
    switch (why) {
    case T3R_PWM_WHY_ALL_ONES: return "A WORD OF ALL ONES";
    case T3R_PWM_WHY_ENABLE_CLEAR_BITS: return "ENABLE CLEAR BUT OTHER BITS SET";
    case T3R_PWM_WHY_ABSOLUTE_0_BITS: return "ABSOLUTE 0 PERCENT WITH OTHER BITS";
    case T3R_PWM_WHY_OTHER_BITS: return "BITS OUTSIDE ENABLE, BIT 24 AND THE FIELD";
    case T3R_PWM_WHY_SPECIAL_ZERO_DUTY: return "FIELD 236, A VALUE HEKATE NEVER WRITES";
    case T3R_PWM_WHY_OUT_OF_RANGE: return "FIELD ABOVE 236";
    default: return "";
    }
}

static void fan_line(struct switch_t3r_line *out, const struct switch_t3r_result *r)
{
    place(out, SWITCH_T3R_COLUMN_ROWS + 1);
    out->right = 0;
    switch_diag_line_add(&out->text, "T3R FAN PWM_CSR_1");
    if (r->gate != T3R_GATE_OPEN) {
        switch_diag_line_add(&out->text, r->gate == T3R_GATE_IN_RESET ? " NOT READ. PWM BLOCK IN RESET" : " NOT READ. PWM CLOCK OFF");
        out->bad = 1;
        return;
    }
    switch_diag_line_add(&out->text, " ");
    switch_diag_line_hex(&out->text, r->value[T3R_PWM_CSR_1], 8);
    switch_diag_line_add(&out->text, ":");
    switch_diag_line_add(&out->text, pwm_text(r->pwm));
    if (r->pwm == T3R_PWM_DUTY) {
        switch_diag_line_add(&out->text, " ");
        switch_diag_line_decimal(&out->text, r->pwm_duty);
        switch_diag_line_add(&out->text, " OF 236");
    }
    if (r->pwm == T3R_PWM_UNKNOWN) {
        switch_diag_line_add(&out->text, ". ");
        switch_diag_line_add(&out->text, why_text(r->pwm_why));
        out->bad = 1;
    }
}

static void route_line(struct switch_t3r_line *out, const struct switch_t3r_result *r)
{
    place(out, SWITCH_T3R_COLUMN_ROWS + 2);
    out->right = 0;
    switch_diag_line_add(&out->text, "T3R ROUTE LCD_GPIO2 ");
    switch_diag_line_hex(&out->text, r->value[T3R_PINMUX_LCD_GPIO2], 8);
    switch_diag_line_add(&out->text, " (OFF STATE 00000074). CAM1_PWDN ");
    switch_diag_line_hex(&out->text, r->value[T3R_PINMUX_CAM1_PWDN], 8);
    switch_diag_line_add(&out->text, " (TACH PULL UP 00000059)");
}

static void rail_line(struct switch_t3r_line *out, const struct switch_t3r_result *r)
{
    place(out, SWITCH_T3R_COLUMN_ROWS + 3);
    out->right = 0;
    switch_diag_line_add(&out->text, "T3R RAIL A5");
    bit_text(&out->text, " CNF ", r->value[T3R_GPIO_A_CNF], 5);
    bit_text(&out->text, " OE ", r->value[T3R_GPIO_A_OE], 5);
    bit_text(&out->text, " OUT ", r->value[T3R_GPIO_A_OUT], 5);
    switch_diag_line_add(&out->text, ". CC4");
    bit_text(&out->text, " CNF ", r->value[T3R_GPIO_CC_CNF], 4);
    bit_text(&out->text, " OE ", r->value[T3R_GPIO_CC_OE], 4);
    bit_text(&out->text, " OUT ", r->value[T3R_GPIO_CC_OUT], 4);
    switch_diag_line_add(&out->text, ". PMC BIT 21");
    bit_text(&out->text, " NO_IOPOWER ", r->value[T3R_PMC_NO_IOPOWER], 21);
    bit_text(&out->text, " PWR_DET_VAL ", r->value[T3R_PMC_PWR_DET_VAL], 21);
}

static void tach_line(struct switch_t3r_line *out, const struct switch_t3r_result *r)
{
    place(out, SWITCH_T3R_COLUMN_ROWS + 4);
    out->right = 0;
    switch_diag_line_add(&out->text, "T3R TACH S7");
    bit_text(&out->text, " CNF ", r->value[T3R_GPIO_S_CNF], 7);
    bit_text(&out->text, " OE ", r->value[T3R_GPIO_S_OE], 7);
    bit_text(&out->text, " IN ", r->value[T3R_GPIO_S_IN], 7);
    switch_diag_line_add(&out->text, ". V4");
    bit_text(&out->text, " CNF ", r->value[T3R_GPIO_V_CNF], 4);
    bit_text(&out->text, " OE ", r->value[T3R_GPIO_V_OE], 4);
    switch_diag_line_add(&out->text, ". ONE SAMPLE IS NOT AN EDGE COUNT");
}

void switch_t3r_screen_line(unsigned step, const struct switch_t3r_result *r, struct switch_t3r_line *out)
{
    out->bad = 0;
    out->row = 0;
    out->right = 0;
    switch_diag_line_start(&out->text, "");
    if (step >= SWITCH_T3R_STEP_BEGIN(0) && step < T3R_STEP_SKIP_PWM) {
        const unsigned index = (step - 1) / 2;
        const unsigned reg = switch_t3r_sequence(index);
        if (step % 2) {
            head(out, index, reg);
            switch_diag_line_add(&out->text, " READ BEGIN 32 BIT");
            pad(out);
        } else {
            result_line(out, r, index, reg);
        }
        return;
    }
    switch (step) {
    case T3R_STEP_START: switch_diag_line_add(&out->text, "T3R START. EL1 STATE PASS"); break;
    case T3R_STEP_SKIP_PWM:
        head(out, 3, T3R_PWM_CSR_1);
        switch_diag_line_add(&out->text, r->gate == T3R_GATE_IN_RESET ? " NOT READ. IN RESET" : " NOT READ. CLOCK OFF");
        out->bad = 1;
        pad(out);
        break;
    case T3R_STEP_FAN: fan_line(out, r); break;
    case T3R_STEP_ROUTE: route_line(out, r); break;
    case T3R_STEP_RAIL: rail_line(out, r); break;
    case T3R_STEP_TACH: tach_line(out, r); break;
    default:
        place(out, SWITCH_T3R_COLUMN_ROWS + 5);
        out->right = 0;
        switch_diag_line_add(&out->text, "T3R COMPLETE. READS ");
        switch_diag_line_decimal(&out->text, r->reads);
        switch_diag_line_add(&out->text, ". STOP ");
        switch_diag_line_hex(&out->text, (uint32_t)r->stop, 4);
        switch_diag_line_add(&out->text, ". NO WRITES");
        out->bad = r->stop != 0;
        break;
    }
}
