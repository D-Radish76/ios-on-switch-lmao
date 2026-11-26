#include "t2_report.h"

const char *switch_t2_stop_name(int stop)
{
    switch (stop) {
    case 0:
        return "COMPLETE";
    case SWITCH_T2_I2C_CLOCK_OFF:
        return "I2C1 CLOCK OFF. NOTHING WRITTEN";
    case SWITCH_T2_I2C_IN_RESET:
        return "I2C1 IN RESET. NOTHING WRITTEN";
    case SWITCH_T2_SOURCE_UNEXPECTED:
        return "I2C1 CLOCK SOURCE IS NOT 00000003. NOTHING WRITTEN";
    case SWITCH_T2_BUS_BUSY:
        return "I2C1 BUSY BEFORE THE DIVISOR WRITE. NOTHING WRITTEN";
    case SWITCH_T2_DIVISOR_UNEXPECTED:
        return "I2C1 DIVISOR IS NOT 00050001. NOTHING WRITTEN";
    case SWITCH_T2_TIMING_ZERO:
        return "I2C1 TIMING REGISTER READS 0. NOTHING WRITTEN";
    case SWITCH_T2_TIMING_UNEXPECTED:
        return "I2C1 TIMING WORD IS NOT THE ONE T1-T READ. NOTHING WRITTEN";
    case SWITCH_T2_TIMING_UNREADABLE:
        return "I2C1 TIMING REGISTER READS FFFFFFFF. NOTHING WRITTEN";
    case SWITCH_T2_IDLE_FAILED:
        return "I2C1 NOT IDLE ACROSS 3 SAMPLES. NOTHING WRITTEN";
    case SWITCH_T2_CONFIG_LOAD_TIMEOUT:
        return "I2C1 CONFIG LOAD DID NOT FINISH IN 1 MS. NO RESTORE";
    case SWITCH_T2_DIVISOR_READBACK:
        return "I2C1 DIVISOR READBACK DIFFERS. NO RESTORE";
    case SWITCH_T2_NOT_IDLE:
        return "I2C1 BUSY WHEN IT SHOULD BE IDLE";
    case SWITCH_T2_DENIED:
    case T2_BUS_DENIED:
        return "ACCESS REFUSED BY THE ALLOWLIST. LOADER BUG";
    case SWITCH_T2_NOT_ERISTA:
        return "NOT AN ERISTA. NO T2 REGISTER TOUCHED";
    case SWITCH_T2_COUNTER_STALLED:
    case T2_BUS_COUNTER_STALLED:
        return "COUNTER DID NOT ADVANCE. NOTHING WRITTEN AFTER IT";
    case SWITCH_T2_INT_PENDING:
        return "I2C1 INTERRUPT STATUS NOT 0. NOTHING WRITTEN";
    case SWITCH_T2_INT_CHANGED:
        return "I2C1 INTERRUPT STATUS CHANGED AFTER THE LOAD. NO RESTORE";
    case SWITCH_T2_LOAD_BEFORE_UNEXPECTED:
        return "CONFIG LOAD IS NOT 00000004 BEFORE THE DIVISOR WRITE. NOTHING WRITTEN";
    case SWITCH_T2_LOAD_BEFORE_PENDING:
        return "CONFIG LOAD BIT 0 SET BEFORE THE DIVISOR WRITE. NOTHING WRITTEN";
    case SWITCH_T2_LOAD_AFTER_UNEXPECTED:
        return "CONFIG LOAD WORD UNEXPECTED AFTER A LOAD";
    case SWITCH_T2_PLLP_UNEXPECTED:
        return "PLLP_BASE IS NOT M 8 N 85 P 1. NOTHING WRITTEN";
    case T2_STOP_READ_FAILED:
        return "A TMP451 REGISTER READ FAILED. NO RETRY";
    case T2_STOP_ID:
        return "MANUFACTURER ID IS NOT 55. NO SENSOR WRITE";
    case T2_STOP_CONFIG:
        return "TMP451 CONFIGURATION IS NOT C0 (SHUTDOWN). NO SENSOR WRITE";
    case T2_STOP_BUSY_BEFORE:
        return "TMP451 STATUS SHOWS BUSY BEFORE THE ONE-SHOT. NO SENSOR WRITE";
    case T2_STOP_WRITE_FAILED:
        return "THE ONE-SHOT WRITE FAILED. NO RETRY";
    case T2_STOP_TIMEOUT:
        return "CONVERSION NOT DONE IN 100 MS. NO RETRY";
    case T2_BUS_NOT_STARTED:
        return "BUSY NEVER SEEN AFTER GO. NOT KNOWN TO HAVE STARTED. NO RETRY";
    case T2_BUS_BUSY_TIMEOUT:
        return "I2C1 STILL BUSY AFTER 100 MS. NO RETRY";
    case T2_BUS_NOACK:
        return "NO ACK FROM 0X4C. NO RETRY";
    case T2_BUS_LOAD_TIMEOUT:
        return "I2C1 TRANSFER LOAD DID NOT FINISH IN 1 MS";
    case T2_BUS_LOAD_UNEXPECTED:
        return "CONFIG LOAD WORD UNEXPECTED AFTER A TRANSFER LOAD. NO GO WRITTEN";
    default:
        return "UNKNOWN";
    }
}

static void word(struct switch_diag_line *line, const char *label, uint32_t value)
{
    switch_diag_line_add(line, label);
    switch_diag_line_hex(line, value, 8);
}

static void byte(struct switch_diag_line *line, const char *label, unsigned value)
{
    switch_diag_line_add(line, label);
    switch_diag_line_hex(line, value, 2);
}

static void sample(struct switch_diag_line *line, const char *label, const struct switch_t2_sample *s)
{
    switch_diag_line_add(line, label);
    byte(line, "REMOTE ", s->remote_high);
    byte(line, ".", s->remote_low);
    byte(line, " LOCAL ", s->local_high);
    byte(line, ".", s->local_low);
}

/* Sixteenths to "NN.NNNN"; the standard range is 0 to 127 C. */
static void degrees(struct switch_diag_line *line, int sixteenths)
{
    switch_diag_line_decimal(line, (unsigned)sixteenths / 16u);
    switch_diag_line_add(line, ".");
    const unsigned fraction = ((unsigned)sixteenths % 16u) * 625u;
    char text[5] = {(char)('0' + fraction / 1000), (char)('0' + fraction / 100 % 10), (char)('0' + fraction / 10 % 10),
                    (char)('0' + fraction % 10), 0};
    switch_diag_line_add(line, text);
}

void switch_t2_lines(const struct switch_t2_image_result *r, struct switch_t2_line lines[SWITCH_T2_LINES])
{
    for (unsigned i = 0; i < SWITCH_T2_LINES; ++i) {
        switch_diag_line_start(&lines[i].text, "");
        lines[i].bad = 0;
    }
    const struct switch_t2_probe *p = &r->probe;
    const struct switch_t2_result *s = &r->sensor;
    word(&lines[0].text, "CAR RST_L ", p->rst_devices_l);
    word(&lines[0].text, " ENB_L ", p->clk_out_enb_l);
    word(&lines[0].text, " SRC_I2C1 ", p->source_i2c1);
    word(&lines[0].text, " PLLP_BASE ", p->pllp_base);
    word(&lines[1].text, "BEFORE CNFG ", p->cnfg);
    word(&lines[1].text, " STATUS ", p->status);
    word(&lines[1].text, " INT ", p->int_status);
    word(&lines[1].text, " DIV ", p->divisor);
    word(&lines[1].text, " LOAD ", p->config_load);
    word(&lines[2].text, "TIMING_0 ", p->timing_0);
    word(&lines[2].text, " TIMING_1 ", p->timing_1);
    if (r->writes) {
        word(&lines[3].text, "AFTER LOAD ", p->load_after);
        word(&lines[3].text, " DIV ", p->divisor_after);
        word(&lines[3].text, " STATUS ", p->status_load);
        word(&lines[3].text, " INT ", p->int_status_load);
        switch_diag_line_add(&lines[3].text, ". DIV WRITTEN 00FF0001, NOT RESTORED");
    }
    if (r->sensor_ran) {
        byte(&lines[4].text, "TMP451 0X4C ID(FE) ", s->id);
        byte(&lines[4].text, " CONFIG(03) ", s->config_before);
        byte(&lines[4].text, " STATUS(02) ", s->status_before);
        byte(&lines[4].text, " OFFSET(11,12) ", s->offset_high);
        byte(&lines[4].text, ".", s->offset_low);
        sample(&lines[5].text, "BEFORE THE ONE-SHOT ", &s->old_sample);
        if (s->writes) {
            switch_diag_line_add(&lines[6].text, "ONE-SHOT WRITTEN. BUSY ");
            if (s->busy_seen) {
                switch_diag_line_add(&lines[6].text, "SEEN AT ");
                switch_diag_line_decimal(&lines[6].text, s->busy_first_us);
                switch_diag_line_add(&lines[6].text, " US");
            } else {
                switch_diag_line_add(&lines[6].text, "NOT SEEN");
            }
            switch_diag_line_add(&lines[6].text, ". DONE AT ");
            if (s->done_us) {
                switch_diag_line_decimal(&lines[6].text, s->done_us);
                switch_diag_line_add(&lines[6].text, " US (MIN 31000, BOUND 100000)");
            } else {
                switch_diag_line_add(&lines[6].text, "NOTHING");
            }
            byte(&lines[6].text, ". LAST STATUS ", s->status_last);
        }
        if (s->done_us) {
            sample(&lines[7].text, "AFTER ", &s->new_sample);
            byte(&lines[7].text, ". CONFIG ", s->config_after);
            switch_diag_line_add(&lines[7].text, s->not_in_shutdown ? " SHUTDOWN BIT CLEAR" : " BACK IN SHUTDOWN");
            lines[7].bad = s->inconsistent || s->not_in_shutdown;
            if (s->inconsistent)
                switch_diag_line_add(&lines[8].text, "THE TWO REMOTE HIGH READS DIFFER. NO TEMPERATURE SHOWN");
            else if ((s->status_last & T2_STATUS_OPEN) || switch_t2_at_range_edge(s->new_sample.remote_high)) {
                switch_diag_line_add(&lines[8].text, "REMOTE DIODE OPEN OR READING AT THE RANGE EDGE. NOT A READING");
                lines[8].bad = 1;
            } else {
                switch_diag_line_add(&lines[8].text, "REMOTE ");
                degrees(&lines[8].text, switch_t2_sixteenths(s->new_sample.remote_high, s->new_sample.remote_low));
                switch_diag_line_add(&lines[8].text, " C, LOCAL ");
                degrees(&lines[8].text, switch_t2_sixteenths(s->new_sample.local_high, s->new_sample.local_low));
                switch_diag_line_add(&lines[8].text, " C. ONE READING. NOT A LIMIT AND NOT A SAFETY CRITERION");
            }
        }
    }
    switch_diag_line_add(&lines[12].text, r->stop ? "T2 STOPPED CODE " : "T2 COMPLETE CODE ");
    switch_diag_line_hex(&lines[12].text, (uint64_t)r->stop, 4);
    switch_diag_line_add(&lines[12].text, ". ");
    switch_diag_line_add(&lines[12].text, switch_t2_stop_name(r->stop));
    lines[12].bad = r->stop != 0;
    switch_diag_line_add(&lines[13].text, "WRITES ");
    switch_diag_line_decimal(&lines[13].text, r->writes);
    switch_diag_line_add(&lines[13].text, ". TRANSFERS ");
    switch_diag_line_decimal(&lines[13].text, r->transfers);
    switch_diag_line_add(&lines[13].text, ". NO RETRY. NO CAR WRITE. ONE SENSOR WRITE AT MOST");
}
