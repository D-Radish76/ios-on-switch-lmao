#include "t1r_report.h"

static void word(struct switch_diag_line *line, const char *label, uint32_t value)
{
    switch_diag_line_add(line, label);
    switch_diag_line_hex(line, value, 8);
}

static void source_line(struct switch_t1r_line *out, unsigned bus, uint32_t value)
{
    const uint32_t expected = switch_t1r_hekate_source(bus);
    switch_diag_line_add(&out->text, "CLK_SOURCE_I2C");
    switch_diag_line_decimal(&out->text, bus + 1);
    word(&out->text, " ", value);
    switch_diag_line_add(&out->text, " FIELD 31:30 ");
    switch_diag_line_hex(&out->text, value >> 30, 1);
    switch_diag_line_add(&out->text, " DIVIDER ");
    switch_diag_line_hex(&out->text, value & 0xffffu, 4);
    word(&out->text, ". HEKATE WRITES ", expected);
    switch_diag_line_add(&out->text, value == expected ? ". SAME" : ". DIFFERENT");
    out->bad = value != expected;
}

void switch_t1r_lines(const struct switch_t1r_result *r, struct switch_t1r_line lines[SWITCH_T1R_LINES])
{
    for (unsigned i = 0; i < SWITCH_T1R_LINES; ++i) {
        switch_diag_line_start(&lines[i].text, "");
        lines[i].bad = 0;
    }
    word(&lines[0].text, "T1R P3 LOADER AT EL1. CAR RST_L ", r->rst_devices_l);
    word(&lines[0].text, " ENB_L ", r->clk_out_enb_l);
    switch_diag_line_add(&lines[0].text, ". READ ONLY");
    switch_diag_line_add(&lines[1].text, (r->clk_out_enb_l & (1u << 12)) ? "I2C1 CLOCK ON" : "I2C1 CLOCK OFF");
    switch_diag_line_add(&lines[1].text, (r->rst_devices_l & (1u << 12)) ? " IN RESET" : " OUT OF RESET");
    switch_diag_line_add(&lines[1].text, ". MUX FIELD: 0 PLL_P, 3 CLK_M (LINUX, NOT THE TRM)");
    for (unsigned bus = 0; bus < SWITCH_T1R_BUSES; ++bus)
        source_line(&lines[2 + bus], bus, r->source[bus]);
    word(&lines[8].text, "PLLP BASE ", r->pllp_base);
    word(&lines[8].text, " OUTA ", r->pllp_outa);
    word(&lines[8].text, " OUTB ", r->pllp_outb);
    switch_diag_line_add(&lines[8].text, ". RAW WORDS. NO FREQUENCY CLAIMED");
    if (r->i2c1 == SWITCH_T1R_I2C1_READ) {
        word(&lines[9].text, "I2C1 CNFG ", r->cnfg);
        word(&lines[9].text, " STATUS ", r->status);
        word(&lines[9].text, " INT ", r->int_status);
        word(&lines[9].text, " DIV ", r->divisor);
        word(&lines[9].text, " LOAD ", r->config_load);
    } else {
        switch_diag_line_add(&lines[9].text, r->i2c1 == SWITCH_T1R_I2C1_CLOCK_OFF
                                                 ? "I2C1 REGISTERS NOT READ: CLOCK OFF"
                                                 : "I2C1 REGISTERS NOT READ: IN RESET");
        lines[9].bad = 1;
    }
    switch_diag_line_add(&lines[10].text, r->stop ? "T1R STOPPED CODE " : "T1R COMPLETE CODE ");
    switch_diag_line_hex(&lines[10].text, (uint64_t)r->stop, 4);
    switch_diag_line_add(&lines[10].text, r->stop ? ". ALLOWLIST REFUSED A READ. LOADER BUG"
                                                  : ". READ ONLY. NO I2C TRANSFER. NO REGISTER WRITE");
    lines[10].bad = r->stop != 0;
}
