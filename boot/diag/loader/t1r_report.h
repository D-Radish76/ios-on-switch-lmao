#ifndef SWITCH_DIAG_T1R_REPORT_H
#define SWITCH_DIAG_T1R_REPORT_H
#include "report.h"
#include "t1r.h"

#define SWITCH_T1R_LINES 11u

struct switch_t1r_line {
    struct switch_diag_line text;
    int bad;
};

/* Fills SWITCH_T1R_LINES lines; an empty one is not drawn. PLLP is shown as raw words and no frequency is claimed. */
void switch_t1r_lines(const struct switch_t1r_result *result, struct switch_t1r_line lines[SWITCH_T1R_LINES]);
#endif
