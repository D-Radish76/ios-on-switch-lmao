#ifndef SWITCH_DIAG_T4R_REPORT_H
#define SWITCH_DIAG_T4R_REPORT_H
#include "report.h"
#include "t4r.h"

struct switch_t4r_line {
    struct switch_diag_line text;
    int bad;
};

/* The screen line of one step, from what the result holds when the step is shown. */
void switch_t4r_screen_line(unsigned step, const struct switch_t4r_result *result, struct switch_t4r_line *out);
#endif
