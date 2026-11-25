#ifndef SWITCH_DIAG_T1V3_REPORT_H
#define SWITCH_DIAG_T1V3_REPORT_H
#include "report.h"
#include "t1v3.h"

#define SWITCH_T1V3_LINES 12u

struct switch_t1v3_line {
    struct switch_diag_line text;
    int bad;
};

/* Fills SWITCH_T1V3_LINES lines; an empty one is not drawn. The TMP451 byte is printed as hex only. */
void switch_t1v3_lines(const struct switch_t1v3_result *result, struct switch_t1v3_line lines[SWITCH_T1V3_LINES]);
const char *switch_t1v3_stop_name(int stop);
#endif
