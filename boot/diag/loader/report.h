#ifndef SWITCH_DIAG_REPORT_H
#define SWITCH_DIAG_REPORT_H
#include <stdint.h>

/* display_text draws at most 104 characters. */
#define SWITCH_DIAG_LINE_MAX 104u

struct switch_diag_line {
    char text[SWITCH_DIAG_LINE_MAX + 1];
    unsigned length;
};

void switch_diag_line_start(struct switch_diag_line *line, const char *text);
/* Cut the line to at most length characters. */
void switch_diag_line_clip(struct switch_diag_line *line, unsigned length);
void switch_diag_line_add(struct switch_diag_line *line, const char *text);
void switch_diag_line_hex(struct switch_diag_line *line, uint64_t value, unsigned digits);
void switch_diag_line_decimal(struct switch_diag_line *line, uint64_t value);
/* "ADDRESS VALUE SAME", or "ADDRESS VALUE DIFFERENT EXPECTED" */
void switch_diag_register_line(struct switch_diag_line *line, uint32_t address, uint32_t value, uint32_t expected);
#endif
