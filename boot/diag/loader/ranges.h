#ifndef SWITCH_DIAG_RANGES_H
#define SWITCH_DIAG_RANGES_H
#include <stdint.h>

/* What the normal loader keeps clear of the live FDT's reservations, so D0 can say whether the Erista's FDT agrees. The
 * values are loader_abi.py's; diag_loader_test.py checks each. */
#define SWITCH_DIAG_SD_REFERENCE UINT64_C(0xa001f000)
#define SWITCH_DIAG_STAGE_START  UINT64_C(0xaa100000)
#define SWITCH_DIAG_STAGE_END    UINT64_C(0xb0000000)
#define SWITCH_DIAG_BOOT_ARGS    UINT64_C(0x8fe00000)
#define SWITCH_DIAG_KERNEL_START UINT64_C(0xb2000000)
#define SWITCH_DIAG_KERNEL_END   UINT64_C(0xe2200000)
#endif
