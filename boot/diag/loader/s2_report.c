#include "s2_report.h"

static void begin(struct switch_s2_line *out, const char *what, unsigned reg)
{
    switch_diag_line_add(&out->text, what);
    switch_diag_line_add(&out->text, " READ BEGIN. ADDRESS ");
    switch_diag_line_hex(&out->text, switch_s2_address(reg), 8);
}

static void result_line(struct switch_s2_line *out, const char *what, uint32_t value)
{
    switch_diag_line_add(&out->text, what);
    switch_diag_line_add(&out->text, " READ RESULT ");
    switch_diag_line_hex(&out->text, value, 8);
}

static void gate(struct switch_s2_line *out, const struct switch_s2_result *r)
{
    switch_diag_line_add(&out->text, "S2 SDMMC1 CLOCK/RESET GATE. ");
    switch_diag_line_add(&out->text, r->sd_state == SWITCH_S2_SD_CLOCK_OFF  ? "CLOCK OFF. NOT READ"
                                     : r->sd_state == SWITCH_S2_SD_IN_RESET ? "IN RESET. NOT READ"
                                                                           : "CLOCK ON. RESET OFF. READ");
}

void switch_s2_screen_line(unsigned step, const struct switch_s2_result *r, struct switch_s2_line *out)
{
    out->bad = 0;
    switch_diag_line_start(&out->text, "");
    switch (step) {
    case S2_STEP_START:
        switch_diag_line_add(&out->text, "S2 START. EL1 STATE PASS");
        break;
    case S2_STEP_CAR_RST_BEGIN: begin(out, "S2 CAR RST", S2_CAR_RST_DEVICES_L); break;
    case S2_STEP_CAR_RST_RESULT: result_line(out, "S2 CAR RST", r->rst_devices_l); break;
    case S2_STEP_CAR_CLK_BEGIN: begin(out, "S2 CAR CLK", S2_CAR_CLK_OUT_ENB_L); break;
    case S2_STEP_CAR_CLK_RESULT: result_line(out, "S2 CAR CLK", r->clk_out_enb_l); break;
    case S2_STEP_GATE: gate(out, r); break;
    case S2_STEP_PRESENT_BEGIN: begin(out, "S2 SDMMC1 PRESENT STATE", S2_SDMMC1_PRESENT_STATE); break;
    case S2_STEP_PRESENT_RESULT: result_line(out, "S2 SDMMC1 PRESENT STATE", r->sd_present_state); break;
    case S2_STEP_CLOCK_WORD_BEGIN: begin(out, "S2 SDMMC1 CLOCK WORD", S2_SDMMC1_WORD_2C); break;
    case S2_STEP_CLOCK_WORD_RESULT: result_line(out, "S2 SDMMC1 CLOCK WORD", r->sd_word_2c); break;
    default:
        switch_diag_line_add(&out->text, "S2 COMPLETE. READS ");
        switch_diag_line_decimal(&out->text, r->reads);
        switch_diag_line_add(&out->text, ". STOP ");
        switch_diag_line_hex(&out->text, (uint32_t)r->stop, 4);
        out->bad = r->stop != 0;
        break;
    }
}
