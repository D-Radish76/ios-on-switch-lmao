#ifndef SWITCH_LOADER_CARVEOUT_H
#define SWITCH_LOADER_CARVEOUT_H
#include <stdint.h>
#include "SwitchBoard.h"

// The memory controller registers native_gate compares, in this order: {address, value}. One table per board, read on
// that board, so a board's values never stand in for another's. A board without a table is refused.
// tools/mc_carveout/test_dump.py and diag_boot.py read switch_carveout_oled.
#define SWITCH_CARVEOUT_REGISTERS 25

static const uint32_t switch_carveout_oled[][2] = {
    {0x70019050, 0x1000},     {0x70019670, 0},          {0x70019674, 0},          {0x700199d4, 0},
    {0x700199a0, 0xfff00000}, {0x700199a4, 0},          {0x700199a8, 0},          {0x70019648, 0},
    {0x7001964c, 0},          {0x70019978, 0},          {0x70019c0c, 0xffe00000}, {0x70019c10, 0},
    {0x70019c14, 8},          {0x70019c5c, 0xffd00000}, {0x70019c60, 0},          {0x70019c64, 2},
    {0x70019cac, 0xffd40000}, {0x70019cb0, 0},          {0x70019cb4, 6},          {0x70019cfc, 0},
    {0x70019d00, 0},          {0x70019d04, 0},          {0x70019d4c, 0},          {0x70019d50, 0},
    {0x70019d54, 0},
};

// Read on the Erista by the D0 loader diagnostic (boot/diag/erista-first-run.log).
static const uint32_t switch_carveout_erista[][2] = {
    {0x70019050, 0x1000},     {0x70019670, 0},          {0x70019674, 0},          {0x700199d4, 0},
    {0x700199a0, 0xfff00000}, {0x700199a4, 0},          {0x700199a8, 0},          {0x70019648, 0},
    {0x7001964c, 0},          {0x70019978, 0},          {0x70019c0c, 0xffe00000}, {0x70019c10, 0},
    {0x70019c14, 8},          {0x70019c5c, 0xffd00000}, {0x70019c60, 0},          {0x70019c64, 2},
    {0x70019cac, 0xffd40000}, {0x70019cb0, 0},          {0x70019cb4, 6},          {0x70019cfc, 0},
    {0x70019d00, 0},          {0x70019d04, 0},          {0x70019d4c, 0},          {0x70019d50, 0},
    {0x70019d54, 0},
};

typedef uint32_t (*switch_carveout_read)(uint32_t address);

static inline const uint32_t (*switch_carveout_table(const char *board))[2]
{
    if (switch_board_streq(board, "oled"))
        return switch_carveout_oled;
    if (switch_board_streq(board, "erista"))
        return switch_carveout_erista;
    return 0;
}

// 0 when every register holds the board's value. 0x402 for a board with no table, 0x410 + i for the first register that
// differs. Reads only.
static inline uint64_t switch_carveout_check(const char *board, switch_carveout_read read)
{
    const uint32_t(*table)[2] = switch_carveout_table(board);
    if (!table)
        return 0x402;
    for (unsigned i = 0; i < SWITCH_CARVEOUT_REGISTERS; ++i)
        if (read(table[i][0]) != table[i][1])
            return 0x410 + i;
    return 0;
}
#endif
