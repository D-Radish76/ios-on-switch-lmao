#ifndef SWITCH_DIAG_T1T_REPORT_H
#define SWITCH_DIAG_T1T_REPORT_H
#include "report.h"
#include "t1t.h"

#define SWITCH_T1T_LINES 16u

struct switch_t1t_line {
    struct switch_diag_line text;
    int bad;
};

/* Fills SWITCH_T1T_LINES lines; an empty one is not drawn. Every word is printed raw, then as its fields. */
void switch_t1t_lines(const struct switch_t1t_result *result, struct switch_t1t_line lines[SWITCH_T1T_LINES]);
#endif
