#ifndef SWITCH_DIAG_SD_REPORT_H
#define SWITCH_DIAG_SD_REPORT_H
#include <stdint.h>
#include "../loader/report.h"
#include "../../storage/sd_reference.h"

/* The status block the D1 boot script fills in, in the exporter's run area above its image and stack. The offsets and
 * the magic are exporter_check.py's; diag_sd_loader_test.py checks each. CRC32 words are as U-Boot's crc32 stores them,
 * big endian. Flags are 1 once their script step passed. */
#define SWITCH_DIAG_SD_STATUS         UINT64_C(0xa700f000)
#define SWITCH_DIAG_SD_STATUS_MAGIC   0x31445353u
#define SWITCH_DIAG_SD_STATUS_VERSION 1u
#define SWITCH_DIAG_SD_FLAGS_OFFSET   0x20u
#define SWITCH_DIAG_SD_FLAG_COUNT     10u
#define SWITCH_DIAG_SD_LINES          6u

struct switch_diag_sd_line {
    struct switch_diag_line text;
    int bad;
};

struct switch_diag_sd_result {
    unsigned flags_set;
    int status_ok;
    int reference_error;
    struct switch_diag_sd_line line[SWITCH_DIAG_SD_LINES];
};

/* Reads the status block and the reference page and fills the result; neither is written and neither is used as an
 * address. The card's identification (CID, CSD words) and the firmware addresses in the record never go into a line. */
void switch_diag_sd_evaluate(const volatile uint8_t *status, const volatile uint8_t *page,
                             struct switch_diag_sd_result *result);
/* The same for the fixed status block and the fixed reference page. The loader's only reads of either. */
void switch_diag_sd_evaluate_fixed(struct switch_diag_sd_result *result);
const char *switch_diag_sd_error_name(int error);
#endif
