#include <stdint.h>
#include "SwitchBoard.h"
#include "checks.h"
#include "diag.h"
#include "display.h"
#include "mc_table.h"
#include "memory.h"
#include "ranges.h"
#include "record.h"
#include "report.h"

_Static_assert(SWITCH_DIAG_FB_PA == DISPLAY_BASE && SWITCH_DIAG_FB_BYTES == DISPLAY_BYTES,
               "the checked framebuffer is the one display.h draws on");

extern volatile uint64_t probe_record[R_RECORD_WORDS];
extern char __image_start[], __image_end[];
extern void probe_halt(void) __attribute__((noreturn));

enum {
    D_FDT_STATUS = 48,
    D_SCREEN_STATUS,
    D_BOARD,
    D_MC_READ,
    D_MC_DIFFERENT,
    D_WALK_CODE,
    D_WALK_RANGE,
};

#define LEFT             16u
#define RIGHT            720u
#define FIRST_STATUS_ROW 31u

static struct {
    int screen;
    int fdt_ok;
    uint32_t fdt_total;
    uint32_t reservations;
    uint64_t fdt_status;
    uint64_t screen_status;
    unsigned status_row;
} diag __attribute__((section(".data")));

static struct switch_diag_span image(void)
{
    return (struct switch_diag_span){(uint64_t)(uintptr_t)__image_start, (uint64_t)(uintptr_t)__image_end};
}

/* The only code that forms an address of a device register. It loads and returns. */
__attribute__((noinline)) uint32_t switch_diag_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static void put(unsigned column, unsigned row, const struct switch_diag_line *line, uint32_t color)
{
    if (diag.screen)
        display_text(column, 12 + row * 20, line->text, color);
}

static void put_text(unsigned row, const char *text, uint32_t color)
{
    struct switch_diag_line line;
    switch_diag_line_start(&line, text);
    put(LEFT, row, &line, color);
}

static void say(const char *text, int bad)
{
    put_text(diag.status_row++, text, bad ? DISPLAY_RED : DISPLAY_WHITE);
}

static void check_fdt_and_screen(void)
{
    const uint64_t pointer = probe_record[R_FDT];
    uint64_t status = switch_diag_fdt_pointer(pointer, image());
    int covers = 0;
    if (!status) {
        const volatile uint32_t *fdt = (const volatile uint32_t *)(uintptr_t)pointer;
        const uint32_t magic = __builtin_bswap32(fdt[0]), total = __builtin_bswap32(fdt[1]);
        const uint32_t structure = __builtin_bswap32(fdt[2]), reserve = __builtin_bswap32(fdt[4]);
        status = switch_diag_fdt_header(magic, total, reserve, structure);
        if (!status)
            status = switch_diag_fdt_reservations(
                fdt, reserve, structure,
                (struct switch_diag_span){SWITCH_DIAG_FB_PA, SWITCH_DIAG_FB_PA + SWITCH_DIAG_FB_BYTES},
                &diag.reservations, &covers);
        diag.fdt_total = total;
    }
    diag.fdt_ok = !status;
    diag.fdt_status = status;
    diag.screen_status =
        diag.fdt_ok ? (uint64_t)switch_diag_framebuffer(SWITCH_DIAG_FB_PA, SWITCH_DIAG_FB_BYTES, covers, image())
                    : SWITCH_DIAG_FB_NO_FDT;
    diag.screen = !diag.screen_status;
    probe_record[D_FDT_STATUS] = diag.fdt_status;
    probe_record[D_SCREEN_STATUS] = diag.screen_status;
}

static void begin_screen(void)
{
    if (diag.screen) {
        display_clear();
        diag.status_row = FIRST_STATUS_ROW;
    }
}

int probe_before_transition(void)
{
    volatile uint64_t *r = probe_record;
    check_fdt_and_screen();
    const int code =
        switch_diag_entry_state(r[R_ENTRY_EL], r[R_ENTRY_SCTLR], r[R_X1], r[R_X2], r[R_X3], r[R_MIDR], r[R_CNTFRQ]);
    r[R_FAILURE] = (uint64_t)code;
    if (code) {
        begin_screen();
        struct switch_diag_line line;
        switch_diag_line_start(&line, "D0 ENTRY CHECK FAILED CODE ");
        switch_diag_line_hex(&line, (uint64_t)code, 4);
        switch_diag_line_add(&line, ". NO EL CHANGE. HOLD THE POWER BUTTON");
        put(LEFT, 0, &line, DISPLAY_RED);
        probe_halt();
    }
    r[R_STAGE] = 2;
    return 1;
}

static void hex_line(unsigned column, unsigned row, const char *label, uint64_t value, unsigned digits,
                     const char *tail, uint32_t color)
{
    struct switch_diag_line line;
    switch_diag_line_start(&line, label);
    switch_diag_line_hex(&line, value, digits);
    switch_diag_line_add(&line, tail);
    put(column, row, &line, color);
}

static void report_el1(const struct switch_diag_el1 *el1, int el_ok)
{
    struct switch_diag_line line;
    switch_diag_line_start(&line, "EL1 EL=");
    switch_diag_line_hex(&line, el1->current_el, 1);
    switch_diag_line_add(&line, " SCTLR=");
    switch_diag_line_hex(&line, el1->sctlr, 8);
    switch_diag_line_add(&line, " DAIF=");
    switch_diag_line_hex(&line, el1->daif, 3);
    put(LEFT, 3, &line, DISPLAY_WHITE);
    switch_diag_line_start(&line, "CNTFRQ=");
    switch_diag_line_decimal(&line, el1->cntfrq);
    switch_diag_line_add(&line, " HCR=");
    switch_diag_line_hex(&line, el1->hcr, 8);
    switch_diag_line_add(&line, " CNTHCTL=");
    switch_diag_line_hex(&line, el1->cnthctl, 1);
    switch_diag_line_add(&line, " CNTVOFF=");
    switch_diag_line_hex(&line, el1->cntvoff, 1);
    put(LEFT, 4, &line, DISPLAY_WHITE);
    put_text(5, el_ok ? "EL1 STATE PASS" : "EL1 STATE FAILED. MC AND GIC READS SKIPPED",
             el_ok ? DISPLAY_GREEN : DISPLAY_RED);
}

static void report_board(const struct switch_board *board, uint32_t hidrev, uint32_t odm4)
{
    hex_line(LEFT, 1, "HIDREV ", hidrev, 8, "", DISPLAY_WHITE);
    struct switch_diag_line line;
    switch_diag_line_start(&line, "ODM4 ");
    switch_diag_line_hex(&line, odm4, 8);
    put(300, 1, &line, DISPLAY_WHITE);
    switch_diag_line_start(&line, "BOARD ");
    switch_diag_line_add(&line, board->name);
    switch_diag_line_add(&line, " T210B01=");
    switch_diag_line_decimal(&line, board->t210b01);
    switch_diag_line_add(&line, " SKU=");
    switch_diag_line_decimal(&line, board->sku);
    switch_diag_line_add(&line, " BOOT=");
    switch_diag_line_decimal(&line, board->boot);
    put(LEFT, 2, &line, board->t210b01 == 0xff ? DISPLAY_RED : DISPLAY_WHITE);
}

static void report_cpu(void)
{
    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    const unsigned crc32 = (unsigned)((isar0 >> 16) & 15);
    struct switch_diag_line line;
    switch_diag_line_start(&line, "ISAR0 ");
    switch_diag_line_hex(&line, isar0, 16);
    switch_diag_line_add(&line, crc32 == 1 ? " CRC32 1 OK" : " CRC32 NOT 1");
    put(LEFT, 6, &line, crc32 == 1 ? DISPLAY_WHITE : DISPLAY_RED);
    const uint64_t sp = probe_record[R_ENTRY_SP];
    const int sp_ok = sp >= UINT64_C(0xf0001000) && sp < SWITCH_DIAG_DRAM_END;
    hex_line(LEFT, 7, "ENTRY SP ", sp, 8, sp_ok ? " IN NATIVE_GATE RANGE" : " OUT OF NATIVE_GATE RANGE",
             sp_ok ? DISPLAY_WHITE : DISPLAY_RED);
}

static void report_memory_controller(void)
{
    unsigned different = 0;
    struct switch_diag_line line;
    for (unsigned i = 0; i < SWITCH_DIAG_MC_COUNT; ++i) {
        const uint32_t value = switch_diag_read32(switch_diag_mc[i][0]);
        switch_diag_register_line(&line, switch_diag_mc[i][0], value, switch_diag_mc[i][1]);
        different += value != switch_diag_mc[i][1];
        put(RIGHT, i, &line, value == switch_diag_mc[i][1] ? DISPLAY_WHITE : DISPLAY_RED);
    }
    for (unsigned i = 0; i < SWITCH_DIAG_GIC_COUNT; ++i) {
        switch_diag_line_start(&line, "GIC ");
        switch_diag_line_hex(&line, switch_diag_gic[i], 8);
        switch_diag_line_add(&line, " ");
        switch_diag_line_hex(&line, switch_diag_read32(switch_diag_gic[i]), 8);
        put(RIGHT, (unsigned)SWITCH_DIAG_MC_COUNT + i, &line, DISPLAY_WHITE);
    }
    probe_record[D_MC_READ] = SWITCH_DIAG_MC_COUNT;
    probe_record[D_MC_DIFFERENT] = different;
    switch_diag_line_start(&line, "MC REGISTERS READ ");
    switch_diag_line_decimal(&line, SWITCH_DIAG_MC_COUNT);
    switch_diag_line_add(&line, " DIFFERENT FROM THE OLED ");
    switch_diag_line_decimal(&line, different);
    put(LEFT, 8, &line, different ? DISPLAY_RED : DISPLAY_GREEN);
}

static void report_fdt(void)
{
    struct switch_diag_line line;
    switch_diag_line_start(&line, "FDT ");
    switch_diag_line_hex(&line, probe_record[R_FDT], 8);
    switch_diag_line_add(&line, " TOTAL ");
    switch_diag_line_decimal(&line, diag.fdt_total);
    switch_diag_line_add(&line, " RESERVATIONS ");
    switch_diag_line_decimal(&line, diag.reservations);
    put(LEFT, 10, &line, DISPLAY_WHITE);
    if (!diag.fdt_ok) {
        hex_line(LEFT, 11, "FDT CHECK CODE ", diag.fdt_status, 4, ". MEMORY WALK SKIPPED", DISPLAY_RED);
        return;
    }
    static const char *const names[] = {"LOADER IMAGE", "SD REFERENCE PAGE", "STAGED WINDOW CAPACITY",
                                        "KERNEL DESTINATION", "BOOT ARGS PAGE"};
    struct loader_memory_range ranges[5];
    ranges[0] = (struct loader_memory_range){UINT64_C(0xa0000000), (uint64_t)(uintptr_t)__image_end};
    ranges[1] = (struct loader_memory_range){SWITCH_DIAG_SD_REFERENCE, SWITCH_DIAG_SD_REFERENCE + 0x1000};
    ranges[2] = (struct loader_memory_range){SWITCH_DIAG_STAGE_START, SWITCH_DIAG_STAGE_END};
    ranges[3] = (struct loader_memory_range){SWITCH_DIAG_KERNEL_START, SWITCH_DIAG_KERNEL_END};
    ranges[4] = (struct loader_memory_range){SWITCH_DIAG_BOOT_ARGS, SWITCH_DIAG_BOOT_ARGS + 0x1000};
    struct loader_memory_report memory;
    unsigned index = 0;
    const int walk = loader_check_fdt_memory_ranges((const uint8_t *)(uintptr_t)probe_record[R_FDT],
                                                    (uint32_t)SWITCH_DIAG_FDT_BYTES, ranges, 5, &memory, &index);
    probe_record[D_WALK_CODE] = (uint64_t)walk;
    probe_record[D_WALK_RANGE] = index;
    switch_diag_line_start(&line, "FDT WALK CODE ");
    switch_diag_line_decimal(&line, (uint64_t)walk);
    switch_diag_line_add(&line, walk ? " REFUSED" : " ALL RANGES FREE");
    put(LEFT, 11, &line, walk ? DISPLAY_RED : DISPLAY_GREEN);
    if (walk) {
        const int overlap = walk == 4 || walk == 20;
        switch_diag_line_start(&line, overlap ? "RESERVATION OVERLAPS " : "FDT STRUCTURE NOT WHAT THE LOADER EXPECTS");
        if (overlap)
            switch_diag_line_add(&line, names[index]);
        put(LEFT, 12, &line, DISPLAY_RED);
    }
    switch_diag_line_start(&line, "FDT NODES ");
    switch_diag_line_decimal(&line, memory.reserved_nodes);
    switch_diag_line_add(&line, " BANKS ");
    switch_diag_line_decimal(&line, memory.bank_count);
    put(LEFT, 13, &line, DISPLAY_WHITE);
}

void switch_diag_after_transition(void)
{
    volatile uint64_t *r = probe_record;
    const struct switch_diag_el1 el1 = {r[R_EL1_EL],    r[R_EL1_SCTLR],     r[R_EL1_DAIF],     r[R_EL1_CNTFRQ],
                                        r[R_HCR_AFTER], r[R_CNTHCTL_AFTER], r[R_CNTVOFF_AFTER]};
    const int el_ok = !switch_diag_el1_state(&el1);
    begin_screen();
    put_text(0, "LOAD DIAG D0. READ ONLY. NO KERNEL. NO BOOT", DISPLAY_GREEN);
    if (diag.screen) {
        const uint32_t hidrev = switch_diag_read32(0x70000804), odm4 = switch_diag_read32(0x7000f9d8);
        const struct switch_board *board = switch_board_detect(hidrev, odm4);
        const int known = board->t210b01 != 0xff;
        r[D_BOARD] = known ? (uint64_t)board->t210b01 << 8 | board->sku : UINT64_MAX;
        report_board(board, hidrev, odm4);
        report_el1(&el1, el_ok);
        report_cpu();
        if (el_ok && known)
            report_memory_controller();
        else
            put_text(8, "MC AND GIC READS SKIPPED", DISPLAY_RED);
        report_fdt();
        put_text(14, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD", DISPLAY_WHITE);
        put_text(15, "PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
    }
    r[R_STAGE] = 0xd0;
    switch_diag_exit(say);
}

void probe_show_exception(uint64_t el, uint64_t esr, uint64_t far, uint64_t pc)
{
    probe_record[R_STAGE] = 0xee;
    if (diag.screen) {
        hex_line(LEFT, 17, "EXCEPTION AT EL ", el, 1, "", DISPLAY_RED);
        hex_line(LEFT, 18, "ESR ", esr, 8, "", DISPLAY_RED);
        hex_line(LEFT, 19, "FAR ", far, 16, "", DISPLAY_RED);
        hex_line(LEFT, 20, "PC ", pc, 16, "", DISPLAY_RED);
    }
    switch_diag_exit(say);
}
