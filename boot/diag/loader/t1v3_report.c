#include "t1v3_report.h"

const char *switch_t1v3_stop_name(int stop)
{
    switch (stop) {
    case 0:
        return "COMPLETE";
    case SWITCH_T1V3_I2C_CLOCK_OFF:
        return "I2C1 CLOCK OFF. NOTHING WRITTEN";
    case SWITCH_T1V3_I2C_IN_RESET:
        return "I2C1 IN RESET. NOTHING WRITTEN";
    case SWITCH_T1V3_SOURCE_UNEXPECTED:
        return "I2C1 CLOCK SOURCE IS NOT 00000003. NOTHING WRITTEN";
    case SWITCH_T1V3_BUS_BUSY:
        return "I2C1 BUSY BEFORE THE DIVISOR WRITE. NOTHING WRITTEN";
    case SWITCH_T1V3_DIVISOR_UNEXPECTED:
        return "I2C1 DIVISOR IS NOT 00050001. NOTHING WRITTEN";
    case SWITCH_T1V3_TIMING_ZERO:
        return "I2C1 TIMING REGISTER READS 0. NOTHING WRITTEN";
    case SWITCH_T1V3_TIMING_UNEXPECTED:
        return "I2C1 TIMING_0 IS NOT 00000204. NOTHING WRITTEN";
    case SWITCH_T1V3_TIMING_UNREADABLE:
        return "I2C1 TIMING REGISTER READS FFFFFFFF. NOTHING WRITTEN";
    case SWITCH_T1V3_IDLE_FAILED:
        return "I2C1 NOT IDLE ACROSS 3 SAMPLES. NOTHING WRITTEN";
    case SWITCH_T1V3_CONFIG_LOAD_TIMEOUT:
        return "I2C1 CONFIG LOAD DID NOT FINISH IN 1 MS. NO RESTORE";
    case SWITCH_T1V3_DIVISOR_READBACK:
        return "I2C1 DIVISOR READBACK DIFFERS. NO RESTORE";
    case SWITCH_T1V3_BUSY_TIMEOUT:
        return "I2C1 STILL BUSY AFTER 100 MS. NO RETRY";
    case SWITCH_T1V3_NOACK:
        return "NO ACK FROM 0X4C. NO RETRY";
    case SWITCH_T1V3_ID_MISMATCH:
        return "MANUFACTURER ID IS NOT 55";
    case SWITCH_T1V3_NOT_IDLE:
        return "I2C1 BUSY WHEN IT SHOULD BE IDLE";
    case SWITCH_T1V3_DENIED:
        return "ACCESS REFUSED BY THE ALLOWLIST. LOADER BUG";
    case SWITCH_T1V3_NOT_ERISTA:
        return "NOT AN ERISTA. NO T1V3 REGISTER TOUCHED";
    case SWITCH_T1V3_COUNTER_STALLED:
        return "COUNTER DID NOT ADVANCE. NO DEVICE TOUCHED OR WRITTEN AFTER IT";
    case SWITCH_T1V3_INT_PENDING:
        return "I2C1 INTERRUPT STATUS NOT 0. NOTHING WRITTEN";
    case SWITCH_T1V3_INT_CHANGED:
        return "I2C1 INTERRUPT STATUS CHANGED AFTER THE LOAD. NO RESTORE";
    default:
        return "UNKNOWN";
    }
}

static void word(struct switch_diag_line *line, const char *label, uint32_t value)
{
    switch_diag_line_add(line, label);
    switch_diag_line_hex(line, value, 8);
}

static void transfer(struct switch_diag_line *line, const char *label, const struct switch_t1v3_transaction *t)
{
    switch_diag_line_add(line, label);
    switch_diag_line_decimal(line, t->elapsed_us);
    switch_diag_line_add(line, t->busy_seen ? " US BUSY SEEN" : " US BUSY NOT SEEN");
}

void switch_t1v3_lines(const struct switch_t1v3_result *r, struct switch_t1v3_line lines[SWITCH_T1V3_LINES])
{
    for (unsigned i = 0; i < SWITCH_T1V3_LINES; ++i) {
        switch_diag_line_start(&lines[i].text, "");
        lines[i].bad = 0;
    }
    word(&lines[0].text, "CAR RST_L ", r->rst_devices_l);
    word(&lines[0].text, " ENB_L ", r->clk_out_enb_l);
    word(&lines[0].text, " SRC_I2C1 ", r->source_i2c1);
    switch_diag_line_add(&lines[0].text, " (EXPECT 00000003)");
    word(&lines[1].text, "BEFORE CNFG ", r->cnfg);
    word(&lines[1].text, " STATUS ", r->status);
    word(&lines[1].text, " INT ", r->int_status);
    word(&lines[1].text, " DIV ", r->divisor);
    word(&lines[1].text, " LOAD ", r->config_load);
    word(&lines[2].text, "TIMING_0 ", r->timing_0);
    word(&lines[2].text, " TIMING_1 ", r->timing_1);
    switch_diag_line_add(&lines[2].text, ". READ ONLY. TIMING_0 EXPECT 00000204");
    if (r->writes) {
        word(&lines[3].text, "AFTER LOAD ", r->load_after);
        word(&lines[3].text, " DIV ", r->divisor_after);
        word(&lines[3].text, " STATUS ", r->status_load);
        word(&lines[3].text, " INT ", r->int_status_load);
        switch_diag_line_add(&lines[3].text, ". DIV WRITTEN 00FF0001, NOT RESTORED");
    }
    if (r->transfers || r->transaction[0].elapsed_us) {
        transfer(&lines[4].text, "WRITE POINTER ", &r->transaction[0]);
        transfer(&lines[4].text, ". READ ", &r->transaction[1]);
        switch_diag_line_add(&lines[4].text, ". ELAPSED IS A REFERENCE, NOT AN SCL MEASUREMENT");
    }
    if (r->have_id) {
        switch_diag_line_add(&lines[5].text, "TMP451 0X4C ID(FE) ");
        switch_diag_line_hex(&lines[5].text, r->id, 2);
        switch_diag_line_add(&lines[5].text, " EXPECT 55. COMMUNICATION CHECK ONLY");
        lines[5].bad = r->id != SWITCH_T1V3_ID_VALUE;
    }
    switch_diag_line_add(&lines[6].text, r->stop ? "T1V3 STOPPED CODE " : "T1V3 COMPLETE CODE ");
    switch_diag_line_hex(&lines[6].text, (uint64_t)r->stop, 4);
    switch_diag_line_add(&lines[6].text, ". ");
    switch_diag_line_add(&lines[6].text, switch_t1v3_stop_name(r->stop));
    lines[6].bad = r->stop != 0;
    switch_diag_line_add(&lines[7].text, "WRITES ");
    switch_diag_line_decimal(&lines[7].text, r->writes);
    switch_diag_line_add(&lines[7].text, ". TRANSFERS ");
    switch_diag_line_decimal(&lines[7].text, r->transfers);
    switch_diag_line_add(&lines[7].text, " OF 2. NO RETRY. NO CAR WRITE. NO SENSOR WRITE. STATUS AFTER ");
    switch_diag_line_hex(&lines[7].text, r->status_after, 8);
    switch_diag_line_add(&lines[7].text, " INT ");
    switch_diag_line_hex(&lines[7].text, r->int_status_after, 8);
}
