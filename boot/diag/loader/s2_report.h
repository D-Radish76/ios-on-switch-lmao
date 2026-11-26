#ifndef SWITCH_DIAG_S2_REPORT_H
#define SWITCH_DIAG_S2_REPORT_H
#include "report.h"
#include "s2.h"

struct switch_s2_line {
    struct switch_diag_line text;
    int bad;
};

/* The screen line of one step, from what the result holds when the step is shown. */
void switch_s2_screen_line(unsigned step, const struct switch_s2_result *result, struct switch_s2_line *out);
#endif
