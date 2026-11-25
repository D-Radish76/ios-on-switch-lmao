#include "sd_report.h"

static uint32_t le32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

const char *switch_diag_sd_error_name(int error)
{
    static const char *const names[] = {"OK", "ARGUMENT", "HEADER", "PADDING", "CRC", "CHANGED", "CARD"};
    return error >= 0 && error < (int)(sizeof names / sizeof names[0]) ? names[error] : "UNKNOWN";
}

static void start(struct switch_diag_sd_line *line, const char *text, int bad)
{
    switch_diag_line_start(&line->text, text);
    line->bad = bad;
}

static void steps_line(struct switch_diag_sd_result *result, int block_ok, uint32_t version)
{
    struct switch_diag_sd_line *line = &result->line[0];
    if (!block_ok) {
        start(line, "SD STATUS BLOCK MISSING OR WRONG VERSION ", 1);
        switch_diag_line_hex(&line->text, version, 8);
        return;
    }
    start(line, "SD STEPS ", result->flags_set != SWITCH_DIAG_SD_FLAG_COUNT);
    switch_diag_line_decimal(&line->text, result->flags_set);
    switch_diag_line_add(&line->text, " OF ");
    switch_diag_line_decimal(&line->text, SWITCH_DIAG_SD_FLAG_COUNT);
    switch_diag_line_add(&line->text, ". EXPORTER RETURNED 0 PER SCRIPT");
}

static void crc_lines(struct switch_diag_sd_result *result, const volatile uint8_t *status)
{
    const uint32_t want = be32(status + 0x08), slot = be32(status + 0x10), run = be32(status + 0x14),
                   go = be32(status + 0x18);
    struct switch_diag_sd_line *line = &result->line[1];
    start(line, "CRC32 WANT ", want == 0);
    switch_diag_line_hex(&line->text, want, 8);
    switch_diag_line_add(&line->text, " BYTES ");
    switch_diag_line_hex(&line->text, le32(status + 0x0c), 4);
    line = &result->line[2];
    start(line, "CRC32 SLOT ", slot != want || run != want || go != want);
    switch_diag_line_hex(&line->text, slot, 8);
    switch_diag_line_add(&line->text, " RUN ");
    switch_diag_line_hex(&line->text, run, 8);
    switch_diag_line_add(&line->text, " GO ");
    switch_diag_line_hex(&line->text, go, 8);
}

static void card_lines(struct switch_diag_sd_result *result, const struct switch_sd_reference *reference,
                       const struct switch_sd_card *card)
{
    struct switch_diag_sd_line *line = &result->line[4];
    start(line, "CARD SECTORS ", 0);
    switch_diag_line_hex(&line->text, card->capacity_sectors, 8);
    switch_diag_line_add(&line->text, " OCR ");
    switch_diag_line_hex(&line->text, card->ocr, 8);
    switch_diag_line_add(&line->text, " RCA ");
    switch_diag_line_hex(&line->text, card->rca, 4);
    const int signature = reference->sector[510] == 0x55 && reference->sector[511] == 0xaa;
    line = &result->line[5];
    start(line, "CSD STRUCT ", !signature);
    switch_diag_line_decimal(&line->text, card->csd[0] >> 30);
    switch_diag_line_add(&line->text, " FLAGS ");
    switch_diag_line_hex(&line->text, card->version, 8);
    switch_diag_line_add(&line->text, signature ? " SIG 55AA" : " NO 55AA");
    switch_diag_line_add(&line->text, ". ID NOT SHOWN");
}

void switch_diag_sd_evaluate(const volatile uint8_t *status, const volatile uint8_t *page,
                             struct switch_diag_sd_result *result)
{
    result->flags_set = 0;
    result->reference_error = -1;
    for (unsigned i = 0; i < SWITCH_DIAG_SD_LINES; ++i)
        start(&result->line[i], "", 0);
    const uint32_t version = le32(status + 4);
    result->status_ok = le32(status) == SWITCH_DIAG_SD_STATUS_MAGIC && version == SWITCH_DIAG_SD_STATUS_VERSION;
    if (result->status_ok)
        for (unsigned i = 0; i < SWITCH_DIAG_SD_FLAG_COUNT; ++i)
            result->flags_set += le32(status + SWITCH_DIAG_SD_FLAGS_OFFSET + 4 * i) == 1;
    steps_line(result, result->status_ok, version);
    if (result->status_ok)
        crc_lines(result, status);
    struct switch_sd_reference reference;
    struct switch_sd_card card;
    result->reference_error = switch_sd_card_reference_validate(page, SWITCH_SD_REFERENCE_BYTES, 0, &reference, &card);
    struct switch_diag_sd_line *line = &result->line[3];
    start(line, result->reference_error ? "SD RECORD AND REFERENCE: FAILED CODE " : "SD RECORD AND REFERENCE: VALID",
          result->reference_error != 0);
    if (result->reference_error) {
        switch_diag_line_decimal(&line->text, (uint64_t)result->reference_error);
        switch_diag_line_add(&line->text, " ");
        switch_diag_line_add(&line->text, switch_diag_sd_error_name(result->reference_error));
        return;
    }
    card_lines(result, &reference, &card);
}

void switch_diag_sd_evaluate_fixed(struct switch_diag_sd_result *result)
{
    switch_diag_sd_evaluate((const volatile uint8_t *)(uintptr_t)SWITCH_DIAG_SD_STATUS,
                            (const volatile uint8_t *)(uintptr_t)SWITCH_SD_REFERENCE_PA, result);
}
