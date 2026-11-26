#ifndef SWITCH_LOADER_CARVEOUT_H
#define SWITCH_LOADER_CARVEOUT_H
#include <stddef.h>
#include <stdint.h>
#include "SwitchBoard.h"

// The memory controller registers native_gate compares, in this order, and the value each board's console held. The
// offsets are the register list and are shared; each board has its own values, read on that board, so one board's
// readings never stand in for another's. A board with no values is refused.
// tools/mc_carveout/test_dump.py and diag_boot.py read switch_carveout_offset and switch_carveout_OLED.
#define SWITCH_CARVEOUT_REGISTERS 25

// Offsets from the memory controller's base: half the room of full addresses, and all 25 registers are in its 4 KiB.
#define SWITCH_CARVEOUT_MC_BASE 0x70019000u
static const uint16_t switch_carveout_offset[SWITCH_CARVEOUT_REGISTERS] = {
    0x050, 0x670, 0x674, 0x9d4, 0x9a0, 0x9a4, 0x9a8, 0x648, 0x64c, 0x978, 0xc0c, 0xc10, 0xc14,
    0xc5c, 0xc60, 0xc64, 0xcac, 0xcb0, 0xcb4, 0xcfc, 0xd00, 0xd04, 0xd4c, 0xd50, 0xd54,
};

static const uint32_t switch_carveout_OLED[SWITCH_CARVEOUT_REGISTERS] = {
    0x1000, 0, 0, 0, 0xfff00000, 0, 0, 0, 0, 0, 0xffe00000, 0, 8, 0xffd00000, 0, 2, 0xffd40000, 0, 6, 0, 0, 0, 0, 0, 0,
};

// Read on the Erista by the D0 loader diagnostic (boot/diag/erista-first-run.log).
static const uint32_t switch_carveout_ERISTA[SWITCH_CARVEOUT_REGISTERS] = {
    0x1000, 0, 0, 0, 0xfff00000, 0, 0, 0, 0, 0, 0xffe00000, 0, 8, 0xffd00000, 0, 2, 0xffd40000, 0, 6, 0, 0, 0, 0, 0, 0,
};

// One entry per row of SWITCH_BOARDS, in its order. A row that does not boot gets none: the loader refuses it before
// native_gate, and its values would only take room below U-Boot's boot timing slot (stamps.h). A row set to boot
// brings its values in with it; carveout_test.cpp (SWITCH_CARVEOUT_ALL_ROWS) checks every row's values whatever its flag.
#ifdef SWITCH_CARVEOUT_ALL_ROWS
#define SWITCH_CARVEOUT_ROW(id, name, t210b01, sku, boot, features, model, compatible) switch_carveout_##id,
#else
#define SWITCH_CARVEOUT_ROW(id, name, t210b01, sku, boot, features, model, compatible) \
    (boot) ? switch_carveout_##id : NULL,
#endif
static const uint32_t *const switch_carveout_values[] = {SWITCH_BOARDS(SWITCH_CARVEOUT_ROW)};
#undef SWITCH_CARVEOUT_ROW
#ifdef __cplusplus
#define SWITCH_CARVEOUT_ASSERT static_assert
#else
#define SWITCH_CARVEOUT_ASSERT _Static_assert
#endif
SWITCH_CARVEOUT_ASSERT(sizeof(switch_carveout_values) / sizeof(switch_carveout_values[0]) ==
                   sizeof(switch_boards) / sizeof(switch_boards[0]),
                       "one carveout entry per board row");

typedef uint32_t (*switch_carveout_read)(uint32_t address);

// The board's values, or NULL for a pointer that is not one of the table's rows (the unknown board included).
static inline const uint32_t *switch_carveout_for(const struct switch_board *board)
{
    const size_t row = (size_t)(board - switch_boards);
    return board >= switch_boards && row < sizeof(switch_carveout_values) / sizeof(switch_carveout_values[0])
               ? switch_carveout_values[row]
               : NULL;
}

// 0 when every register holds the board's value. 0x402 for a board with no values, 0x410 + i for the first register
// that differs. Reads only.
static inline uint64_t switch_carveout_check(const struct switch_board *board, switch_carveout_read read)
{
    const uint32_t *expected = switch_carveout_for(board);
    if (!expected)
        return 0x402;
    for (unsigned i = 0; i < SWITCH_CARVEOUT_REGISTERS; ++i)
        if (read(SWITCH_CARVEOUT_MC_BASE + switch_carveout_offset[i]) != expected[i])
            return 0x410 + i;
    return 0;
}
#endif
