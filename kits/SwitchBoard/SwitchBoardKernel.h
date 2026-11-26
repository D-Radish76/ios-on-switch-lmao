// SPDX-License-Identifier: BSD-2-Clause-Patent
// The board the kernel is running on (kernel only). The registers, the loader's switch-board=<name> word (appended after
// bootargs.txt; XNU keeps the last of two equal boot-args, so the card's file cannot change it) and the device tree's
// compatible list must agree on one row of the table; anything else is the unknown board, which has no features. A kernel
// started without the loader has no word and is believed to be the OLED only where the registers say it is one.
#ifndef SWITCH_BOARD_KERNEL_H
#define SWITCH_BOARD_KERNEL_H

#include <pexpert/pexpert.h>

#include "SwitchBoard.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SWITCH_BOARD_HOST_SHIM
int switch_board_read_hardware(uint32_t *hidrev, uint32_t *odm4);
int switch_board_read_compatible(const char **list, size_t *bytes);
#else
#include <pexpert/device_tree.h>

#include "../SwitchCAR/SwitchCAR.h"
#include "../SwitchMMIO/SwitchMMIOKernel.h"

// APB_MISC's HIDREV and the fuse cache's FUSE_RESERVED_ODM4, which are only readable with the fuse clock on.
static inline int switch_board_read_hardware(uint32_t *hidrev, uint32_t *odm4)
{
    volatile uint8_t *misc = switch_mmio_map(0x70000000, 0x1000);
    volatile uint8_t *fuse = switch_mmio_map(0x7000f000, 0x1000);
    if (!misc || !fuse)
        return -1;
    switch_car_fuse_readable();
    *hidrev = switch_mmio_read32(misc, 0x804);
    *odm4 = switch_mmio_read32(fuse, 0x9d8);
    return 0;
}

static inline int switch_board_read_compatible(const char **list, size_t *bytes)
{
    DTEntry entry;
    const void *property = NULL;
    unsigned int size = 0;
    if (SecureDTLookupEntry(NULL, "/", &entry) != kSuccess ||
        SecureDTGetProperty(entry, "compatible", &property, &size) != kSuccess || !property || !size)
        return -1;
    *list = (const char *)property;
    *bytes = size;
    return 0;
}
#endif

// A word that fills the buffer may have been cut, and no row's name is that long.
static inline const struct switch_board *switch_board_resolve_now(void)
{
    char name[SWITCH_BOARD_NAME_BYTES] = "";
    const char *word = NULL;
    if (PE_parse_boot_arg_str(SWITCH_BOARD_ARG, name, sizeof(name))) {
        if (switch_board_streq(name, "") || name[sizeof(name) - 2] != 0)
            return &switch_board_unknown;
        word = name;
    }
    uint32_t hidrev = 0, odm4 = 0;
    const char *compatible = NULL;
    size_t compatible_bytes = 0;
    if (switch_board_read_hardware(&hidrev, &odm4) || switch_board_read_compatible(&compatible, &compatible_bytes))
        return &switch_board_unknown;
    return switch_board_resolve(word, hidrev, odm4, compatible, compatible_bytes);
}

static inline const struct switch_board *switch_board_current(void)
{
#ifdef SWITCH_BOARD_HOST_SHIM
    return switch_board_resolve_now();
#else
    static const struct switch_board *resolved; // per translation unit: the registers are read once for each
    if (!resolved)
        resolved = switch_board_resolve_now();
    return resolved;
#endif
}

// The features a row has beyond its mask (SwitchBoard.h). The diagnostic images embed switch_boards[] and their hashes are
// pinned, so a bit added to SWITCH_FEATURES_ALL would change them. Every row names its own here, 0 for none: a row
// without a SWITCH_KERNEL_FEATURES_<id> does not compile.
#define SWITCH_KERNEL_FEATURES_OLED (SWITCH_FEATURE_PMIC | SWITCH_FEATURE_MC_ERRORS)
#define SWITCH_KERNEL_FEATURES_ERISTA 0u
#define SWITCH_KERNEL_ROW(id, name, t210b01, sku, boot, features, model, compatible) SWITCH_KERNEL_FEATURES_##id,

// Integer arithmetic: the board is a pointer into a table of this unit or the unknown board, and ordering or subtracting
// pointers into different objects is undefined.
static inline uint32_t switch_board_kernel_features(const struct switch_board *board)
{
    static const uint32_t rows[] = {SWITCH_BOARDS(SWITCH_KERNEL_ROW)};
    const uintptr_t at = (uintptr_t)board - (uintptr_t)switch_boards;
    return at < sizeof(switch_boards) && at % sizeof(switch_boards[0]) == 0 ? rows[at / sizeof(switch_boards[0])] : 0;
}
#undef SWITCH_KERNEL_ROW

static inline int switch_board_has(enum switch_feature feature)
{
    const struct switch_board *board = switch_board_current();
    return ((board->features | switch_board_kernel_features(board)) & (uint32_t)feature) == (uint32_t)feature;
}

#ifdef __cplusplus
}
#endif

#endif
