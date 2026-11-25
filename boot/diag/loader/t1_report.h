#ifndef SWITCH_DIAG_T1_REPORT_H
#define SWITCH_DIAG_T1_REPORT_H
#include "report.h"
#include "t1.h"

#define SWITCH_T1_LINES 8u

struct switch_t1_line {
    struct switch_diag_line text;
    int bad;
};

/* Fills SWITCH_T1_LINES lines; an empty one is not drawn. TMP451 bytes are printed as hex and never as a temperature. */
void switch_t1_lines(const struct switch_t1_result *result, struct switch_t1_line lines[SWITCH_T1_LINES]);
const char *switch_t1_stop_name(int stop);
#endif
