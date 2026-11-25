#include "reservations.h"

#define LIMIT (UINT64_C(1) << (4 * SWITCH_DIAG_ADDRESS_DIGITS))

void switch_diag_range_line(struct switch_diag_line *line, const char *label, uint64_t start, uint64_t end)
{
    switch_diag_line_start(line, label);
    if (end < start || end > LIMIT) {
        switch_diag_line_add(line, " BEYOND 40 BITS");
        return;
    }
    switch_diag_line_add(line, " ");
    switch_diag_line_hex(line, start, SWITCH_DIAG_ADDRESS_DIGITS);
    switch_diag_line_add(line, " ");
    switch_diag_line_hex(line, end - start, SWITCH_DIAG_ADDRESS_DIGITS);
    switch_diag_line_add(line, " ");
    switch_diag_line_hex(line, end, SWITCH_DIAG_ADDRESS_DIGITS);
}

int switch_diag_first_overlap(const struct switch_ram_ledger *ledger, uint64_t start, uint64_t end)
{
    for (unsigned i = 0; i < ledger->count && i < SWITCH_RAM_RANGES; ++i) {
        if (start < ledger->reserved[i].end && ledger->reserved[i].start < end)
            return (int)i;
    }
    return -1;
}

int switch_diag_bank_holding(const struct switch_ram_ledger *ledger, uint64_t start, uint64_t end)
{
    for (int i = 0; i < 2; ++i) {
        if (ledger->banks[i].start <= start && end <= ledger->banks[i].end && start < end)
            return i;
    }
    return -1;
}
