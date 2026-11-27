#include "t3_report.h"

static const char *const state_text[] = {
    [T3_STATE_OK] = "OK",
    [T3_STATE_GATE] = "PWM CLOCK OFF OR IN RESET",
    [T3_STATE_PWM] = "PWM_CSR_1 NOT ZERO",
    [T3_STATE_LCD_PAD] = "LCD_GPIO2 NOT 00000074",
    [T3_STATE_TACH_PAD] = "CAM1_PWDN NOT 00000059",
    [T3_STATE_RAIL_PIN] = "SATA_LED_ACTIVE NOT 00000001",
    [T3_STATE_USB_PIN] = "USB_VBUS_EN0 NOT 00000101",
    [T3_STATE_A5] = "GPIO A5 NOT AN OUTPUT DRIVEN LOW OR A_OUT NOT ZERO",
    [T3_STATE_CC4] = "GPIO CC4 NOT AN OUTPUT DRIVEN LOW",
    [T3_STATE_S7] = "GPIO S7 NOT AN INPUT",
    [T3_STATE_V4] = "GPIO V4 NOT SPECIAL FUNCTION",
    [T3_STATE_PMC] = "PMC NO_IOPOWER NOT ZERO OR PWR_DET_VAL BIT 21 SET",
};

static const char *const window_name[T3_WINDOW_COUNT] = {"IDLE", "RAIL", "SPIN-UP", "SPIN", "STOPPED"};

static void pad(struct switch_t3_line *out)
{
    while (out->text.length < SWITCH_DIAG_LINE_MAX)
        switch_diag_line_add(&out->text, " ");
}

static void window_text(struct switch_diag_line *line, const struct switch_t3_result *r, unsigned id)
{
    const struct switch_t3_window *w = &r->window[id];
    switch_diag_line_add(line, window_name[id]);
    if (!w->ran) {
        switch_diag_line_add(line, " NOT RUN");
        return;
    }
    switch_diag_line_add(line, " ");
    switch_diag_line_decimal(line, w->samples);
    switch_diag_line_add(line, " SAMPLES ");
    switch_diag_line_decimal(line, w->transitions);
    switch_diag_line_add(line, " TRANSITIONS. LEVEL ");
    switch_diag_line_decimal(line, w->first_level);
    switch_diag_line_add(line, " TO ");
    switch_diag_line_decimal(line, w->last_level);
    if (w->cut)
        switch_diag_line_add(line, w->cut == T3_CUT_RAIL ? " CUT BY RAIL LIMIT" : " CUT BY SAMPLE LIMIT");
}

static void state_line(struct switch_t3_line *out, const struct switch_t3_result *r, int finished)
{
    switch_diag_line_add(&out->text, "1 STATE. ");
    if (!finished) {
        switch_diag_line_add(&out->text, "READING 20 REGISTERS. NO WRITE YET");
        return;
    }
    if (r->rail == T3_RAIL_UNCHECKED && r->state == T3_STATE_OK) {
        switch_diag_line_add(&out->text, "READS DID NOT FINISH. RAIL NOT CHECKED. NO WRITE WAS MADE. READS ");
        switch_diag_line_decimal(&out->text, r->reads);
        out->bad = 1;
        return;
    }
    switch_diag_line_add(&out->text, r->state == T3_STATE_OK ? "AS EXPECTED. " : "DIFFERS: ");
    if (r->state != T3_STATE_OK) {
        switch_diag_line_add(&out->text, state_text[r->state]);
        switch_diag_line_add(&out->text, ". NO WRITE WAS MADE. ");
        out->bad = 1;
    }
    switch_diag_line_add(&out->text, "TACH LEVEL AT ENTRY ");
    switch_diag_line_decimal(&out->text, r->baseline_level);
    switch_diag_line_add(&out->text, ". READS ");
    switch_diag_line_decimal(&out->text, r->reads);
}

static void write_text(struct switch_diag_line *line, const struct switch_t3_result *r, unsigned write, const char *label)
{
    switch_diag_line_add(line, label);
    switch_diag_line_add(line, " ");
    if (!(r->issued & 1u << write)) {
        switch_diag_line_add(line, "NOT SENT");
        return;
    }
    switch_diag_line_hex(line, r->readback[write], 8);
    switch_diag_line_add(line, r->mismatched & 1u << write ? " READBACK DIFFERS" : " OK");
}

static void off_line(struct switch_t3_line *out, const struct switch_t3_result *r)
{
    switch_diag_line_add(&out->text, "5 OFF. ");
    write_text(&out->text, r, T3_O_PWM_OFF, "PWM");
    switch_diag_line_add(&out->text, ". ");
    write_text(&out->text, r, T3_O_RAIL_OFF, "RAIL");
    switch_diag_line_add(&out->text, ". ");
    write_text(&out->text, r, T3_O_PARK_PAD, "PAD");
    switch_diag_line_add(&out->text, ". ");
    switch_diag_line_add(&out->text, r->rail == T3_RAIL_OFF_CONFIRMED ? "RAIL OFF CONFIRMED BY LATCH READ"
                                     : r->rail == T3_RAIL_MAYBE_ON    ? "RAIL MAY BE ON. TREAT IT AS ON"
                                     : r->rail == T3_RAIL_UNKNOWN     ? "RAIL STATE UNKNOWN. TREAT IT AS ON"
                                                                      : "RAIL NOT CHECKED. NO WRITE MADE");
    out->bad = r->mismatched >> T3_O_PWM_OFF != 0 || r->rail != T3_RAIL_OFF_CONFIRMED;
}

static void complete_line(struct switch_t3_line *out, const struct switch_t3_result *r)
{
    out->row = 8;
    switch_diag_line_add(&out->text, "RESULT ");
    switch_diag_line_add(&out->text, r->verdict == T3_VERDICT_PASS ? "PASS: MORE TRANSITIONS AT DUTY 51 THAN BEFORE. " : "NO PASS. ");
    switch_diag_line_add(&out->text, "STOP ");
    switch_diag_line_hex(&out->text, (uint32_t)r->stop, 4);
    switch_diag_line_add(&out->text, ". WRITES ");
    switch_diag_line_decimal(&out->text, r->writes);
    switch_diag_line_add(&out->text, ". READS ");
    switch_diag_line_decimal(&out->text, r->reads);
    out->bad = r->verdict != T3_VERDICT_PASS;
}

void switch_t3_screen_line(unsigned step, const struct switch_t3_result *r, struct switch_t3_line *out)
{
    out->bad = 0;
    out->row = 0;
    switch_diag_line_start(&out->text, "");
    if (step == SWITCH_T3_STEP_START) {
        switch_diag_line_add(&out->text, "T3 START. EL1 STATE PASS. FAN DUTY 51 ONLY");
    } else if (step == SWITCH_T3_STEP_COMPLETE) {
        complete_line(out, r);
    } else {
        const unsigned phase = (step - 1) / 2;
        const int finished = step % 2 == 0;
        out->row = 1 + phase;
        switch (phase) {
        case T3_PHASE_STATE: state_line(out, r, finished); break;
        case T3_PHASE_IDLE:
            switch_diag_line_add(&out->text, "2 ");
            if (finished)
                window_text(&out->text, r, T3_WINDOW_IDLE);
            else
                switch_diag_line_add(&out->text, "IDLE WINDOW 2 S. RAIL OFF. PWM OFF");
            break;
        case T3_PHASE_RAIL:
            switch_diag_line_add(&out->text, "3 ");
            if (!finished) {
                switch_diag_line_add(&out->text, "RAIL ON NEXT. GPIO A5 HIGH. WINDOW 2 S");
                break;
            }
            write_text(&out->text, r, T3_W_RAIL_ON, "A5");
            switch_diag_line_add(&out->text, ". ");
            window_text(&out->text, r, T3_WINDOW_RAIL);
            break;
        case T3_PHASE_SPIN:
            switch_diag_line_add(&out->text, "4 ");
            if (!finished) {
                switch_diag_line_add(&out->text, "DUTY 51 NEXT. PWM1 ON THE PAD. SPIN-UP 1 S THEN WINDOW 2 S");
                break;
            }
            write_text(&out->text, r, T3_W_PWM_DUTY, "PWM");
            switch_diag_line_add(&out->text, ". ");
            write_text(&out->text, r, T3_W_ROUTE_PWM, "PAD");
            switch_diag_line_add(&out->text, ". ");
            window_text(&out->text, r, T3_WINDOW_SPIN);
            break;
        case T3_PHASE_OFF: off_line(out, r); break;
        default:
            switch_diag_line_add(&out->text, "6 ");
            if (finished)
                window_text(&out->text, r, T3_WINDOW_STOPPED);
            else
                switch_diag_line_add(&out->text, "STOPPED WINDOW 2 S. RAIL OFF");
            break;
        }
    }
    pad(out);
}
