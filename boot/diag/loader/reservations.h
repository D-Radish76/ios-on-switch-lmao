#ifndef SWITCH_DIAG_RESERVATIONS_H
#define SWITCH_DIAG_RESERVATIONS_H
#include <stdint.h>
#include "../../loader/ram.h"
#include "report.h"

/* D0-R only: where the D1 exporter runs (64 KiB at the address the normal boot script loads it to). */
#define SWITCH_DIAG_EXPORTER_START UINT64_C(0xa7000000)
#define SWITCH_DIAG_EXPORTER_END   UINT64_C(0xa7010000)
#define SWITCH_DIAG_ADDRESS_DIGITS 10u

/* "LABEL START SIZE END", ten hex digits each; "LABEL BEYOND 40 BITS" for a range that does not fit them. */
void switch_diag_range_line(struct switch_diag_line *line, const char *label, uint64_t start, uint64_t end);
/* Index of the first reserved range of the ledger that overlaps [start, end), or -1. */
int switch_diag_first_overlap(const struct switch_ram_ledger *ledger, uint64_t start, uint64_t end);
/* The bank that holds all of [start, end), or -1. */
int switch_diag_bank_holding(const struct switch_ram_ledger *ledger, uint64_t start, uint64_t end);
#endif
