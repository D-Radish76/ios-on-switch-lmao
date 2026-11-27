#ifndef SWITCH_DIAG_T3R_REPORT_H
#define SWITCH_DIAG_T3R_REPORT_H
#include "report.h"
#include "t3r.h"

/* Cells of a read line are this many characters wide; the left and right column each hold 11 reads. */
#define SWITCH_T3R_CELL        50u
#define SWITCH_T3R_COLUMN_ROWS 11u
/* The summary lines use the full width on the rows below the two read columns. */
#define SWITCH_T3R_SUMMARY_FIRST_ROW SWITCH_T3R_COLUMN_ROWS

struct switch_t3r_line {
    struct switch_diag_line text;
    int bad;
    unsigned row;   /* from the first row of the stage's area */
    unsigned right; /* nonzero: the right column */
};

/* The screen line of one step, from what the result holds when the step is shown. A read line is padded to its cell so that
 * the RESULT line covers the BEGIN line it replaces. */
void switch_t3r_screen_line(unsigned step, const struct switch_t3r_result *result, struct switch_t3r_line *out);
#endif
