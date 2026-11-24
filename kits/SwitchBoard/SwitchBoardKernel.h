// SPDX-License-Identifier: BSD-2-Clause-Patent
// The board as the loader told the kernel (kernel only). The loader reads the hardware and appends switch-board=<name> to the
// command line after bootargs.txt; XNU keeps the last of two equal boot-args, so bootargs.txt cannot change it. No
// switch-board at all is a kernel started without this loader (the emulator checks), which has always been the OLED. A name
// that is not in the table is the unknown board, with no features.
#ifndef SWITCH_BOARD_KERNEL_H
#define SWITCH_BOARD_KERNEL_H

#include <pexpert/pexpert.h>

#include "SwitchBoard.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline const struct switch_board *switch_board_current(void)
{
    char name[SWITCH_BOARD_NAME_BYTES] = "";
    if (!PE_parse_boot_arg_str(SWITCH_BOARD_ARG, name, sizeof(name)))
        return switch_board_by_name("oled");
    return switch_board_by_name(name);
}

static inline int switch_board_has(enum switch_feature feature)
{
    return switch_board_has_feature(switch_board_current(), feature);
}

#ifdef __cplusplus
}
#endif

#endif
