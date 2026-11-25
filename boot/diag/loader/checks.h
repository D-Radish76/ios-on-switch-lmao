#ifndef SWITCH_DIAG_CHECKS_H
#define SWITCH_DIAG_CHECKS_H
#include <stdint.h>

#define SWITCH_DIAG_FDT_PA            UINT64_C(0x8d000000)
#define SWITCH_DIAG_FDT_BYTES         UINT64_C(0x40000)
#define SWITCH_DIAG_DRAM_START        UINT64_C(0x80000000)
#define SWITCH_DIAG_DRAM_END          UINT64_C(0xffd00000)
#define SWITCH_DIAG_FB_PA             UINT64_C(0xf5a00000)
#define SWITCH_DIAG_FB_BYTES          UINT64_C(0x384000)
#define SWITCH_DIAG_CNTFRQ            UINT32_C(19200000)
#define SWITCH_DIAG_WAIT_SECONDS      15u
#define SWITCH_DIAG_PSCI_SYSTEM_RESET UINT32_C(0x84000009)
#define SWITCH_DIAG_RESERVATIONS_MAX  64u

struct switch_diag_span {
    uint64_t start, end;
};

struct switch_diag_el1 {
    uint64_t current_el, sctlr, daif, cntfrq, hcr, cnthctl, cntvoff;
};

/* Every check returns 0 or the code shown on the screen. */
enum {
    SWITCH_DIAG_ENTRY_EL = 0x101,
    SWITCH_DIAG_ENTRY_ARGUMENTS = 0x102,
    SWITCH_DIAG_ENTRY_CPU = 0x103,
    SWITCH_DIAG_ENTRY_TIMER = 0x104,
    SWITCH_DIAG_FDT_ADDRESS = 0x1001,
    SWITCH_DIAG_FDT_ALIGNMENT = 0x1002,
    SWITCH_DIAG_FDT_DRAM = 0x1003,
    SWITCH_DIAG_FDT_OVERLAPS_IMAGE = 0x1004,
    SWITCH_DIAG_FDT_MAGIC = 0x1010,
    SWITCH_DIAG_FDT_TOTAL = 0x1011,
    SWITCH_DIAG_FDT_OFFSETS = 0x1020,
    SWITCH_DIAG_FDT_RESERVATION_END = 0x1021,
    SWITCH_DIAG_FDT_RESERVATION_SIZE = 0x1022,
    SWITCH_DIAG_FDT_RESERVATION_COUNT = 0x1023,
    SWITCH_DIAG_FB_GEOMETRY = 0x2001,
    SWITCH_DIAG_FB_DRAM = 0x2002,
    SWITCH_DIAG_FB_OVERLAPS_IMAGE = 0x2003,
    SWITCH_DIAG_FB_NOT_RESERVED = 0x2004,
    SWITCH_DIAG_FB_NO_FDT = 0x2005,
    SWITCH_DIAG_EL1_STATE = 0x301,
};

int switch_diag_entry_state(uint64_t current_el, uint64_t sctlr, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t midr,
                            uint64_t cntfrq);
int switch_diag_el1_state(const struct switch_diag_el1 *state);

/* The page the boot script copies the DTB to, whole, inside DRAM and clear of the loader's image. */
int switch_diag_fdt_pointer(uint64_t pointer, struct switch_diag_span image);
/* The header words, already in host order. reserve and structure are the offsets of the reservation block and the
 * structure block. */
int switch_diag_fdt_header(uint32_t magic, uint32_t total, uint32_t reserve, uint32_t structure);
/* The reservation block, read in place: entries counted, and whether one entry covers all of window. */
int switch_diag_fdt_reservations(const volatile uint32_t *fdt, uint32_t reserve, uint32_t structure,
                                 struct switch_diag_span window, uint32_t *entries, int *covers);

int switch_diag_framebuffer(uint64_t base, uint64_t bytes, int reserved, struct switch_diag_span image);

/* Counter ticks the wait lasts, or 0 when frequency is not the 19.2 MHz this SoC's counter runs at. */
uint64_t switch_diag_wait_ticks(uint64_t frequency, unsigned seconds);
int switch_diag_before(uint64_t now, uint64_t deadline);
#endif
