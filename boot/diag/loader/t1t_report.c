#include "t1t_report.h"

static void word(struct switch_diag_line *line, const char *label, uint32_t value)
{
    switch_diag_line_add(line, label);
    switch_diag_line_hex(line, value, 8);
}

static void field(struct switch_diag_line *line, const char *label, uint32_t value)
{
    switch_diag_line_add(line, label);
    switch_diag_line_decimal(line, value);
}

static const char *const name[SWITCH_T1T_BLOCKS] = {"I2C1", "I2C5"};

static void source_line(struct switch_t1t_line *out, const struct switch_t1t_result *r, unsigned first)
{
    for (unsigned bus = first; bus < first + 3; ++bus) {
        switch_diag_line_add(&out->text, bus == first ? "SRC I2C" : " I2C");
        switch_diag_line_decimal(&out->text, bus + 1);
        word(&out->text, " ", r->source[bus]);
    }
    if (first == 3)
        switch_diag_line_add(&out->text, ". FIELD 31:30 0 PLL_P 3 CLK_M (LINUX, NOT THE TRM)");
}

static void block_lines(struct switch_t1t_line out[4], const struct switch_t1t_block *b, const char *bus)
{
    if (b->state != SWITCH_T1T_BLOCK_READ) {
        switch_diag_line_add(&out[0].text, bus);
        switch_diag_line_add(&out[0].text, b->state == SWITCH_T1T_BLOCK_CLOCK_OFF ? " NOT READ: CLOCK OFF"
                                                                                  : " NOT READ: IN RESET");
        out[0].bad = 1;
        return;
    }
    const uint32_t *w = b->word;
    switch_diag_line_add(&out[0].text, bus);
    word(&out[0].text, " CNFG ", w[T1T_CNFG]);
    word(&out[0].text, " STATUS ", w[T1T_STATUS]);
    field(&out[0].text, " BUSY ", (w[T1T_STATUS] >> 8) & 1u);
    field(&out[0].text, " NOACK ", w[T1T_STATUS] & 0xfu);
    word(&out[0].text, " INT ", w[T1T_INT_STATUS]);
    switch_diag_line_add(&out[1].text, bus);
    word(&out[1].text, " DIV ", w[T1T_CLK_DIVISOR]);
    switch_diag_line_hex(&out[1].text, w[T1T_CLK_DIVISOR] >> 16, 4);
    switch_diag_line_add(&out[1].text, " STD/FAST FIELD, HS ");
    switch_diag_line_hex(&out[1].text, w[T1T_CLK_DIVISOR] & 0xffffu, 4);
    word(&out[1].text, ". LOAD ", w[T1T_CONFIG_LOAD]);
    field(&out[1].text, " BIT0 ", w[T1T_CONFIG_LOAD] & 1u);
    field(&out[1].text, " BIT2 ", (w[T1T_CONFIG_LOAD] >> 2) & 1u);
    field(&out[1].text, " BIT5 ", (w[T1T_CONFIG_LOAD] >> 5) & 1u);
    word(&out[1].text, " OTHER ", w[T1T_CONFIG_LOAD] & ~0x25u);
    switch_diag_line_add(&out[2].text, bus);
    word(&out[2].text, " TIMING_0 ", w[T1T_TIMING_0]);
    field(&out[2].text, " TLOW ", SWITCH_T1T_TLOW(w[T1T_TIMING_0]));
    field(&out[2].text, " THIGH ", SWITCH_T1T_THIGH(w[T1T_TIMING_0]));
    word(&out[2].text, " OTHER BITS ", w[T1T_TIMING_0] & ~SWITCH_T1T_TIMING_0_FIELDS);
    switch_diag_line_add(&out[3].text, bus);
    word(&out[3].text, " TIMING_1 ", w[T1T_TIMING_1]);
    field(&out[3].text, " TSU_STA ", SWITCH_T1T_TSU_STA(w[T1T_TIMING_1]));
    field(&out[3].text, " THD_STA ", SWITCH_T1T_THD_STA(w[T1T_TIMING_1]));
    field(&out[3].text, " TSU_STO ", SWITCH_T1T_TSU_STO(w[T1T_TIMING_1]));
    field(&out[3].text, " TBUF ", SWITCH_T1T_TBUF(w[T1T_TIMING_1]));
    word(&out[3].text, " OTHER BITS ", w[T1T_TIMING_1] & ~SWITCH_T1T_TIMING_1_FIELDS);
}

static void compare_line(struct switch_diag_line *line, const struct switch_t1t_result *r)
{
    const struct switch_t1t_block *a = &r->block[0], *b = &r->block[1];
    if (a->state != SWITCH_T1T_BLOCK_READ || b->state != SWITCH_T1T_BLOCK_READ) {
        switch_diag_line_add(line, "COMPARISON NOT POSSIBLE: ONE CONTROLLER WAS NOT READ");
        return;
    }
    switch_diag_line_add(line, a->word[T1T_TIMING_0] == b->word[T1T_TIMING_0] ? "TIMING_0 EQUAL ON BOTH"
                                                                                : "TIMING_0 DIFFERENT");
    switch_diag_line_add(line, a->word[T1T_TIMING_1] == b->word[T1T_TIMING_1] ? ". TIMING_1 EQUAL ON BOTH"
                                                                                : ". TIMING_1 DIFFERENT");
    switch_diag_line_add(line, ". EQUAL WORDS ARE NOT PROOF OF A RESET VALUE");
}

void switch_t1t_lines(const struct switch_t1t_result *r, struct switch_t1t_line lines[SWITCH_T1T_LINES])
{
    for (unsigned i = 0; i < SWITCH_T1T_LINES; ++i) {
        switch_diag_line_start(&lines[i].text, "");
        lines[i].bad = 0;
    }
    word(&lines[0].text, "T1T LOADER AT EL1. CAR RST_L ", r->rst_devices_l);
    word(&lines[0].text, " ENB_L ", r->clk_out_enb_l);
    word(&lines[0].text, " RST_H ", r->rst_devices_h);
    word(&lines[0].text, " ENB_H ", r->clk_out_enb_h);
    switch_diag_line_add(&lines[1].text, (r->clk_out_enb_l & (1u << 12)) ? "I2C1 CLOCK ON" : "I2C1 CLOCK OFF");
    switch_diag_line_add(&lines[1].text, (r->rst_devices_l & (1u << 12)) ? " IN RESET" : " OUT OF RESET");
    switch_diag_line_add(&lines[1].text, (r->clk_out_enb_h & (1u << 15)) ? ". I2C5 CLOCK ON" : ". I2C5 CLOCK OFF");
    switch_diag_line_add(&lines[1].text, (r->rst_devices_h & (1u << 15)) ? " IN RESET" : " OUT OF RESET");
    switch_diag_line_add(&lines[1].text, ". READ ONLY");
    source_line(&lines[2], r, 0);
    source_line(&lines[3], r, 3);
    word(&lines[4].text, "PLLP_BASE ", r->pllp_base);
    field(&lines[4].text, " M ", SWITCH_T1T_PLLP_M(r->pllp_base));
    field(&lines[4].text, " N ", SWITCH_T1T_PLLP_N(r->pllp_base));
    field(&lines[4].text, " P ", SWITCH_T1T_PLLP_P(r->pllp_base));
    switch_diag_line_add(&lines[4].text, ". RAW WORD AND FIELDS. NO FREQUENCY CLAIMED");
    for (unsigned b = 0; b < SWITCH_T1T_BLOCKS; ++b)
        block_lines(&lines[5 + 4 * b], &r->block[b], name[b]);
    compare_line(&lines[13].text, r);
    switch_diag_line_add(&lines[14].text, r->stop ? "T1T STOPPED CODE " : "T1T COMPLETE CODE ");
    switch_diag_line_hex(&lines[14].text, (uint64_t)r->stop, 4);
    switch_diag_line_add(&lines[14].text, r->stop ? ". ALLOWLIST REFUSED A READ. LOADER BUG"
                                                   : ". READ ONLY. NO I2C TRANSFER. NO REGISTER WRITE");
    lines[14].bad = r->stop != 0;
    switch_diag_line_add(&lines[15].text, "REGISTER READS ");
    switch_diag_line_decimal(&lines[15].text, r->reads);
    switch_diag_line_add(&lines[15].text, ". VALUES ARE CANDIDATES FOR REVIEW, NOT A SAFETY PROOF");
}
