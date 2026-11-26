#include "t4r_report.h"

static void begin(struct switch_t4r_line *out, const char *what, unsigned reg)
{
    switch_diag_line_add(&out->text, what);
    switch_diag_line_add(&out->text, " READ BEGIN. ADDRESS ");
    switch_diag_line_hex(&out->text, switch_t4r_address(reg), 8);
    switch_diag_line_add(&out->text, ". 32 BIT");
}

static void result_line(struct switch_t4r_line *out, const char *what, uint32_t value)
{
    switch_diag_line_add(&out->text, what);
    switch_diag_line_add(&out->text, " READ RESULT ");
    switch_diag_line_hex(&out->text, value, 8);
}

static void second_tmrus(struct switch_t4r_line *out, const struct switch_t4r_result *r)
{
    result_line(out, "T4R TMRUS 2", r->tmrus_second);
    if (switch_t4r_advanced(r->tmrus_first, r->tmrus_second)) {
        switch_diag_line_add(&out->text, ". ADVANCED BY ");
        switch_diag_line_decimal(&out->text, (uint32_t)(r->tmrus_second - r->tmrus_first));
        switch_diag_line_add(&out->text, " US");
    } else {
        switch_diag_line_add(&out->text, ". DID NOT ADVANCE");
        out->bad = 1;
    }
}

static const char *state_name(unsigned state)
{
    switch (state) {
    case T4R_STATE_IDLE: return "IDLE";
    case T4R_STATE_CONFIGURED_STOPPED: return "CONFIGURED, STOPPED";
    case T4R_STATE_DORMANT: return "DORMANT, ENABLED ON A STOPPED TIMER";
    case T4R_STATE_BUSY: return "BUSY, LEFT ALONE";
    case T4R_STATE_UNKNOWN: return "UNKNOWN";
    default: return "NOT CLASSIFIED";
    }
}

static const char *why_text(unsigned why)
{
    switch (why) {
    case T4R_WHY_ALL_ONES: return ". A WORD READ AS ALL ONES";
    case T4R_WHY_STATUS_BITS: return ". STATUS BITS NO SOURCE NAMES";
    case T4R_WHY_TIMER_VALUE: return ". A STOPPED TIMER HOLDS A VALUE";
    case T4R_WHY_OTHER_SOURCE: return ". ENABLED WITH A SOURCE OTHER THAN TIMER 5";
    default: return "";
    }
}

void switch_t4r_screen_line(unsigned step, const struct switch_t4r_result *r, struct switch_t4r_line *out)
{
    out->bad = 0;
    switch_diag_line_start(&out->text, "");
    switch (step) {
    case T4R_STEP_START:
        switch_diag_line_add(&out->text, "T4R START. EL1 STATE PASS");
        break;
    case T4R_STEP_TMRUS1_BEGIN: begin(out, "T4R TMRUS 1", T4R_TMRUS); break;
    case T4R_STEP_TMRUS1_RESULT: result_line(out, "T4R TMRUS 1", r->tmrus_first); break;
    case T4R_STEP_PTV_BEGIN: begin(out, "T4R TMR5 PTV", T4R_TMR5_PTV); break;
    case T4R_STEP_PTV_RESULT: result_line(out, "T4R TMR5 PTV", r->ptv); break;
    case T4R_STEP_CONFIG_BEGIN: begin(out, "T4R WDT0 CONFIG", T4R_WDT0_CONFIG); break;
    case T4R_STEP_CONFIG_RESULT: result_line(out, "T4R WDT0 CONFIG", r->config); break;
    case T4R_STEP_STATUS_BEGIN: begin(out, "T4R WDT0 STATUS", T4R_WDT0_STATUS); break;
    case T4R_STEP_STATUS_RESULT: result_line(out, "T4R WDT0 STATUS", r->status); break;
    case T4R_STEP_TMRUS2_BEGIN: begin(out, "T4R TMRUS 2", T4R_TMRUS); break;
    case T4R_STEP_TMRUS2_RESULT: second_tmrus(out, r); break;
    case T4R_STEP_STATE:
        switch_diag_line_add(&out->text, "T4R WDT0 AFTER HEKATE INIT: ");
        switch_diag_line_add(&out->text, state_name(r->state));
        switch_diag_line_add(&out->text, why_text(r->why));
        out->bad = r->state == T4R_STATE_UNKNOWN;
        break;
    default:
        switch_diag_line_add(&out->text, "T4R COMPLETE. READS ");
        switch_diag_line_decimal(&out->text, r->reads);
        switch_diag_line_add(&out->text, ". STOP ");
        switch_diag_line_hex(&out->text, (uint32_t)r->stop, 4);
        out->bad = r->stop != 0;
        break;
    }
}
