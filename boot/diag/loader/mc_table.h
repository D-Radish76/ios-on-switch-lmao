#ifndef SWITCH_DIAG_MC_TABLE_H
#define SWITCH_DIAG_MC_TABLE_H
#include <stdint.h>

/* The memory controller registers native_gate (boot/loader/carveout.h) compares, in its order, with the values it expects
 * (read on the OLED). Only ever read. diag_loader_test.py keeps both lists equal to the loader's (carveout.h, loader.c). */
static const uint32_t switch_diag_mc[][2] = {
    {0x70019050, 0x1000},     {0x70019670, 0},          {0x70019674, 0},          {0x700199d4, 0},
    {0x700199a0, 0xfff00000}, {0x700199a4, 0},          {0x700199a8, 0},          {0x70019648, 0},
    {0x7001964c, 0},          {0x70019978, 0},          {0x70019c0c, 0xffe00000}, {0x70019c10, 0},
    {0x70019c14, 8},          {0x70019c5c, 0xffd00000}, {0x70019c60, 0},          {0x70019c64, 2},
    {0x70019cac, 0xffd40000}, {0x70019cb0, 0},          {0x70019cb4, 6},          {0x70019cfc, 0},
    {0x70019d00, 0},          {0x70019d04, 0},          {0x70019d4c, 0},          {0x70019d50, 0},
    {0x70019d54, 0},
};
#define SWITCH_DIAG_MC_COUNT (sizeof(switch_diag_mc) / sizeof(switch_diag_mc[0]))

/* The GIC words native_gate records, observation only. */
static const uint32_t switch_diag_gic[] = {0x50041000, 0x50041004, 0x50041100, 0x50042000, 0x50042004, 0x500420fc};
#define SWITCH_DIAG_GIC_COUNT (sizeof(switch_diag_gic) / sizeof(switch_diag_gic[0]))
#endif
