#ifndef SWITCH_DIAG_T3_REPORT_H
#define SWITCH_DIAG_T3_REPORT_H
#include "report.h"
#include "t3.h"

#define SWITCH_T3_LINE_ROWS 10u

struct switch_t3_line {
    struct switch_diag_line text;
    int bad;
    unsigned row; /* from the first row of the stage's area, below SWITCH_T3_LINE_ROWS */
};

/* The screen line of one step, from what the result holds when the step is shown. Every line is padded to the full width so
 * that a report line covers the announcement it replaces. */
void switch_t3_screen_line(unsigned step, const struct switch_t3_result *result, struct switch_t3_line *out);
#endif
