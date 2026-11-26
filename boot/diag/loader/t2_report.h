#ifndef SWITCH_DIAG_T2_REPORT_H
#define SWITCH_DIAG_T2_REPORT_H
#include "report.h"
#include "t2_image.h"

#define SWITCH_T2_LINES 14u

struct switch_t2_line {
    struct switch_diag_line text;
    int bad;
};

/* Fills SWITCH_T2_LINES lines; an empty one is not drawn. A temperature is printed only from a run that completed, and is
 * marked as not a limit. */
void switch_t2_lines(const struct switch_t2_image_result *result, struct switch_t2_line lines[SWITCH_T2_LINES]);
const char *switch_t2_stop_name(int stop);
#endif
