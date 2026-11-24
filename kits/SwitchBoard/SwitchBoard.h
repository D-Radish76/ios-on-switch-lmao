// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which console this is, and what the kernel may start on it. One table for the boot script (tools/package/boot_script.py
// reads SWITCH_BOARDS), the loader and the kernel.
//
// A board is told apart the way the pinned U-Boot (bl33.bin, board late init) does it, so the two always agree:
//   t210b01 = HIDREV major (0x70000804 bits 7:4) is 2
//   sku     = 0 unless t210b01; else FUSE_RESERVED_ODM4 (0x7000F9D8) bits 19:16: 2 gives 2, 4 gives 3, anything else 1
// A row with boot 0 is known but never booted: the script and the loader refuse it. Its features are what the kernel may
// start once it is enabled; every capability that is not in the mask stays off.
#ifndef SWITCH_BOARD_H
#define SWITCH_BOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum switch_feature {
    SWITCH_FEATURE_CHARGER = 1u << 0,  // SwitchBattery: BQ24193 and MAX17050
    SWITCH_FEATURE_USB = 1u << 1,      // SwitchUSB and, started by it, SwitchPD (BM92T36)
    SWITCH_FEATURE_TOUCH = 1u << 2,
    SWITCH_FEATURE_SENSORS = 1u << 3,  // thermalmonitord's sensors, which read the thermal and battery services
    SWITCH_FEATURE_ALS = 1u << 4,
    SWITCH_FEATURE_MOTION = 1u << 5,
    SWITCH_FEATURE_WIRELESS = 1u << 6,  // Wi-Fi, Bluetooth, Joy-Con and the module's 32 kHz clock from the MAX77620
    SWITCH_FEATURE_GPU = 1u << 7,
    SWITCH_FEATURE_VIDEO = 1u << 8,  // NVDEC, NVENC, VIC, NVJPG
    SWITCH_FEATURE_AUDIO = 1u << 9,
    SWITCH_FEATURE_THERMAL = 1u << 10,  // fan, the CPU operating point and so the DFLL and the CPU rail
    SWITCH_FEATURE_EMC = 1u << 11,      // DRAM clock requests to the BPMP
    SWITCH_FEATURE_SLEEP = 1u << 12,
    SWITCH_FEATURE_SE2 = 1u << 13,  // the second security engine, which only the T210B01 has
    SWITCH_FEATURE_RTC_MARK = 1u << 14,  // DEVELOPMENT only: the boot mark in the MAX77620 RTC alarm (a write to the RTC)
    SWITCH_FEATURE_CACHE_IDLE = 1u << 15,  // osfmk/arm64/cpu.c: idle with the L1 and L2 retained (Cortex-A57 only)
};

#define SWITCH_FEATURES_ALL 0xffffu

// X(id, "name", t210b01, sku, boot, features, model, compatible). boot_script.py parses the first five fields.
// model and compatible are what the board's device tree says about itself; NULL is not known yet. Nothing fills in a
// guess for the Erista: it stays NULL until the string is read off the console's own DTB.
#define SWITCH_BOARDS(X)                                                                               \
    X(OLED, "oled", 1, 3, 1, SWITCH_FEATURES_ALL, "Nintendo Switch (OLED model)", "nintendo,aula")    \
    X(ERISTA, "erista", 0, 0, 0, 0, NULL, NULL)

struct switch_board {
    const char *name;
    uint8_t t210b01;
    uint8_t sku;
    uint8_t boot;
    uint32_t features;
    const char *model;
    const char *compatible;
};

// A board that boots in the minimal mode lists no features: every capability is switched on one reviewed bit at a time.
#define SWITCH_BOARD_ROW(id, name, t210b01, sku, boot, features, model, compatible) \
    {name, t210b01, sku, boot, features, model, compatible},
static const struct switch_board switch_boards[] = {SWITCH_BOARDS(SWITCH_BOARD_ROW)};
#undef SWITCH_BOARD_ROW

// What is not in the table: boots nowhere and has no features.
static const struct switch_board switch_board_unknown = {"unknown", 0xff, 0xff, 0, 0, NULL, NULL};

static inline int switch_board_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// t210b01 and sku as U-Boot sets them, from HIDREV and FUSE_RESERVED_ODM4. *t210b01 is 0 or 1.
static inline void switch_board_identify(uint32_t hidrev, uint32_t odm4, uint32_t *t210b01, uint32_t *sku)
{
    *t210b01 = ((hidrev >> 4) & 0xf) == 2;
    if (!*t210b01) {
        *sku = 0;
        return;
    }
    const uint32_t type = (odm4 >> 16) & 0xf;
    *sku = type == 2 ? 2 : type == 4 ? 3 : 1;
}

static inline const struct switch_board *switch_board_find(uint32_t t210b01, uint32_t sku)
{
    for (size_t i = 0; i < sizeof(switch_boards) / sizeof(switch_boards[0]); i++)
        if (switch_boards[i].t210b01 == t210b01 && switch_boards[i].sku == sku)
            return &switch_boards[i];
    return &switch_board_unknown;
}

// The board from the two registers, for the loader: U-Boot calls every chip that is not a T210B01 sku 0, so this also
// requires the T210 family (HIDREV 0x21) and major 1 or 2 before it believes the row.
static inline const struct switch_board *switch_board_detect(uint32_t hidrev, uint32_t odm4)
{
    const uint32_t major = (hidrev >> 4) & 0xf;
    if (((hidrev >> 8) & 0xff) != 0x21 || (major != 1 && major != 2))
        return &switch_board_unknown;
    uint32_t t210b01, sku;
    switch_board_identify(hidrev, odm4, &t210b01, &sku);
    return switch_board_find(t210b01, sku);
}

static inline const struct switch_board *switch_board_by_name(const char *name)
{
    for (size_t i = 0; i < sizeof(switch_boards) / sizeof(switch_boards[0]); i++)
        if (switch_board_streq(switch_boards[i].name, name))
            return &switch_boards[i];
    return &switch_board_unknown;
}

// The row a kernel without the loader's word may still be: the one this tree ran on before the word existed.
#define SWITCH_BOARD_LEGACY "oled"

// Whether the NUL separated list a device tree property holds (compatible) names `wanted`. A list that does not end in a
// zero is cut off or not a string list, and names nothing.
static inline int switch_board_lists(const char *list, size_t bytes, const char *wanted)
{
    if (!list || !bytes || !wanted || list[bytes - 1] != 0)
        return 0;
    for (size_t at = 0; at < bytes;) {
        if (switch_board_streq(list + at, wanted))
            return 1;
        while (list[at])
            at++;
        at++;
    }
    return 0;
}

// The row whose device tree compatible string the list holds, for code that runs before the registers can be read
// (osfmk/arm64/cpu.c). It is only as strong as the device tree: the unknown board when no row is named.
static inline const struct switch_board *switch_board_by_compatible(const char *list, size_t bytes)
{
    for (size_t i = 0; i < sizeof(switch_boards) / sizeof(switch_boards[0]); i++)
        if (switch_boards[i].compatible && switch_board_lists(list, bytes, switch_boards[i].compatible))
            return &switch_boards[i];
    return &switch_board_unknown;
}

// What the kernel believes about the console, from three witnesses that must agree: the registers (HIDREV, ODM4), the
// loader's word (NULL when the command line has none) and the device tree's compatible list. Anything missing, malformed
// or in disagreement is the unknown board. Without a word only the legacy row is believed, and only because the registers
// say so; a row whose device tree strings are not known (NULL) cannot be confirmed and is unknown too.
static inline const struct switch_board *switch_board_resolve(const char *word, uint32_t hidrev, uint32_t odm4,
                                                              const char *compatible, size_t compatible_bytes)
{
    const struct switch_board *board = switch_board_detect(hidrev, odm4);
    if (board == &switch_board_unknown)
        return board;
    if (word ? switch_board_by_name(word) != board : !switch_board_streq(board->name, SWITCH_BOARD_LEGACY))
        return &switch_board_unknown;
    if (!board->compatible || !switch_board_lists(compatible, compatible_bytes, board->compatible))
        return &switch_board_unknown;
    return board;
}

#define SWITCH_BOARD_ARG        "switch-board"
#define SWITCH_BOARD_NAME_BYTES 16
#define SWITCH_BOARD_WORD_BYTES (sizeof(SWITCH_BOARD_ARG) + SWITCH_BOARD_NAME_BYTES)

// "switch-board=<name>" and its zero in out, which the loader appends to the command line after bootargs.txt. Returns the
// bytes written, or 0 when the name does not fit.
static inline size_t switch_board_word(char *out, size_t capacity, const struct switch_board *board)
{
    size_t at = 0;
    for (const char *p = SWITCH_BOARD_ARG "="; *p; p++, at++) {
        if (at + 1 >= capacity)
            return 0;
        out[at] = *p;
    }
    for (const char *p = board->name; *p; p++, at++) {
        if (at + 1 >= capacity || at >= SWITCH_BOARD_WORD_BYTES - 1)
            return 0;
        out[at] = *p;
    }
    out[at++] = 0;
    return at;
}

static inline int switch_board_has_feature(const struct switch_board *board, enum switch_feature feature)
{
    return (board->features & (uint32_t)feature) == (uint32_t)feature;
}

#ifdef __cplusplus
}
#endif

#endif
