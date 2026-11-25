#include "t1_report.h"

const char *switch_t1_stop_name(int stop)
{
    switch (stop) {
    case 0:
        return "COMPLETE";
    case SWITCH_T1_I2C_CLOCK_OFF:
        return "I2C1 CLOCK OFF. NO I2C1 REGISTER TOUCHED";
    case SWITCH_T1_I2C_IN_RESET:
        return "I2C1 IN RESET. NO I2C1 REGISTER TOUCHED";
    case SWITCH_T1_BUS_BUSY:
        return "I2C1 BUSY BEFORE THE FIRST TRANSFER. NO BUS CLEAR";
    case SWITCH_T1_DIVISOR_UNEXPECTED:
        return "I2C1 CLOCK DIVISOR IS NOT HEKATE'S. NOT WRITTEN";
    case SWITCH_T1_CONFIG_LOAD_TIMEOUT:
        return "I2C1 CONFIG LOAD DID NOT FINISH IN 1 MS";
    case SWITCH_T1_BUSY_TIMEOUT:
        return "I2C1 STILL BUSY AFTER 100 MS";
    case SWITCH_T1_NOACK:
        return "NO ACK FROM 0X4C";
    case SWITCH_T1_ID_MISMATCH:
        return "MANUFACTURER ID IS NOT 55";
    case SWITCH_T1_NOT_IDLE:
        return "I2C1 STILL BUSY AFTER THE LAST TRANSFER";
    case SWITCH_T1_DENIED:
        return "ACCESS REFUSED BY THE ALLOWLIST. LOADER BUG";
    case SWITCH_T1_NOT_ERISTA:
        return "NOT AN ERISTA. NO T1 REGISTER TOUCHED";
    case SWITCH_T1_COUNTER_STALLED:
        return "COUNTER DID NOT ADVANCE. NO DEVICE TOUCHED";
    default:
        return "UNKNOWN";
    }
}

static void word(struct switch_diag_line *line, const char *label, uint32_t value)
{
    switch_diag_line_add(line, label);
    switch_diag_line_hex(line, value, 8);
}

static void byte(struct switch_diag_line *line, const struct switch_t1_result *r, unsigned index)
{
    if (r->have & (1u << index))
        switch_diag_line_hex(line, r->value[index], 2);
    else
        switch_diag_line_add(line, "--");
}

void switch_t1_lines(const struct switch_t1_result *r, struct switch_t1_line lines[SWITCH_T1_LINES])
{
    for (unsigned i = 0; i < SWITCH_T1_LINES; ++i) {
        switch_diag_line_start(&lines[i].text, "");
        lines[i].bad = 0;
    }
    word(&lines[0].text, "CAR RST_L ", r->rst_devices_l);
    word(&lines[0].text, " ENB_L ", r->clk_out_enb_l);
    word(&lines[0].text, " SRC_I2C1 ", r->source_i2c1);
    word(&lines[0].text, " SRC_PWM ", r->source_pwm);

    const int i2c_on = switch_t1_i2c1_clocked(r), pwm_on = switch_t1_pwm_clocked(r);
    switch_diag_line_add(&lines[1].text, i2c_on ? "I2C1 CLOCK ON" : "I2C1 CLOCK OFF");
    switch_diag_line_add(&lines[1].text, (r->rst_devices_l & (1u << 12)) ? " IN RESET" : " OUT OF RESET");
    switch_diag_line_add(&lines[1].text, pwm_on ? ". PWM CLOCK ON" : ". PWM CLOCK OFF");
    switch_diag_line_add(&lines[1].text, (r->rst_devices_l & (1u << 17)) ? " IN RESET" : " OUT OF RESET");
    switch_diag_line_add(&lines[1].text, ". CAR READ ONLY, PWM REGISTERS NOT TOUCHED");
    lines[1].bad = !i2c_on || (r->rst_devices_l & (1u << 12));

    if (i2c_on && !(r->rst_devices_l & (1u << 12))) {
        word(&lines[2].text, "I2C1 CNFG ", r->cnfg);
        word(&lines[2].text, " STATUS ", r->status);
        word(&lines[2].text, " INT ", r->int_status);
        word(&lines[2].text, " DIV ", r->divisor);
        word(&lines[2].text, " LOAD ", r->config_load);
        word(&lines[3].text, "PAD SCL ", r->pad_scl);
        word(&lines[3].text, " SDA ", r->pad_sda);
    }
    if (r->have) {
        switch_diag_line_add(&lines[4].text, "TMP451 0X4C ID(FE) ");
        byte(&lines[4].text, r, 0);
        switch_diag_line_add(&lines[4].text, " EXPECT 55. CONFIG(03) ");
        byte(&lines[4].text, r, 1);
        switch_diag_line_add(&lines[4].text, ". RATE(04) ");
        byte(&lines[4].text, r, 2);
        lines[4].bad = (r->have & 1u) && r->value[0] != 0x55;
        switch_diag_line_add(&lines[5].text, "RAW 00 ");
        byte(&lines[5].text, r, 3);
        switch_diag_line_add(&lines[5].text, " RAW 01 ");
        byte(&lines[5].text, r, 4);
        switch_diag_line_add(&lines[5].text, ". COMMUNICATION CHECK ONLY. NOT A TEMPERATURE");
    }
    switch_diag_line_add(&lines[6].text, r->stop ? "T1 STOPPED CODE " : "T1 COMPLETE CODE ");
    switch_diag_line_hex(&lines[6].text, (uint64_t)r->stop, 4);
    switch_diag_line_add(&lines[6].text, ". ");
    switch_diag_line_add(&lines[6].text, switch_t1_stop_name(r->stop));
    lines[6].bad = r->stop != 0;
    switch_diag_line_add(&lines[7].text, "TRANSFERS ");
    switch_diag_line_decimal(&lines[7].text, r->transfers);
    switch_diag_line_add(&lines[7].text, " OF 10. NO RETRY. NO SENSOR WRITE. FAN AND PWM UNTOUCHED. STATUS AFTER ");
    switch_diag_line_hex(&lines[7].text, r->status_after, 8);
}
