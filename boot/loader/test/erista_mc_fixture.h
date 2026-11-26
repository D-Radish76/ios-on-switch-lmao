// The 25 memory controller registers read on a physical Erista by the D0 loader diagnostic, in native_gate's order
// (boot/diag/erista-first-run.log). carveout_fixture_test.py keeps this list equal to that log.
#pragma once
#include <cstdint>

struct MeasuredRegister {
    uint32_t address;
    uint32_t value;
};

static const MeasuredRegister kEristaMeasured[] = {
    {0x70019050, 0x00001000}, {0x70019670, 0x00000000}, {0x70019674, 0x00000000}, {0x700199d4, 0x00000000},
    {0x700199a0, 0xfff00000}, {0x700199a4, 0x00000000}, {0x700199a8, 0x00000000}, {0x70019648, 0x00000000},
    {0x7001964c, 0x00000000}, {0x70019978, 0x00000000}, {0x70019c0c, 0xffe00000}, {0x70019c10, 0x00000000},
    {0x70019c14, 0x00000008}, {0x70019c5c, 0xffd00000}, {0x70019c60, 0x00000000}, {0x70019c64, 0x00000002},
    {0x70019cac, 0xffd40000}, {0x70019cb0, 0x00000000}, {0x70019cb4, 0x00000006}, {0x70019cfc, 0x00000000},
    {0x70019d00, 0x00000000}, {0x70019d04, 0x00000000}, {0x70019d4c, 0x00000000}, {0x70019d50, 0x00000000},
    {0x70019d54, 0x00000000},
};
