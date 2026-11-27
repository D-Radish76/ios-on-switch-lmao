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
#ifdef SWITCH_DIAG_RESERVATIONS
#include "reservations.h"
#endif
#ifdef SWITCH_DIAG_SD
#include "sd_report.h"
#endif
#ifdef SWITCH_DIAG_T1
#include "t1_report.h"
#endif
#ifdef SWITCH_DIAG_T1R
#include "t1r_report.h"
#endif
#ifdef SWITCH_DIAG_T1V3
#include "t1v3_report.h"
#endif
#ifdef SWITCH_DIAG_T1T
#include "t1t_report.h"
#endif
#ifdef SWITCH_DIAG_T2
#include "t2_report.h"
#endif
#ifdef SWITCH_DIAG_S2
#include "s2_report.h"
#endif
#ifdef SWITCH_DIAG_T4R
#include "t4r_report.h"
#endif
#ifdef SWITCH_DIAG_T3R
#include "t3r_report.h"
#endif
#ifdef SWITCH_DIAG_T3
#include "t3_report.h"
#endif

#if defined(SWITCH_DIAG_T1) || defined(SWITCH_DIAG_T1R) || defined(SWITCH_DIAG_T1V3) || \
    defined(SWITCH_DIAG_T1T) || defined(SWITCH_DIAG_T2) || defined(SWITCH_DIAG_S2) || defined(SWITCH_DIAG_T4R) || \
    defined(SWITCH_DIAG_T3R) || defined(SWITCH_DIAG_T3)
#define SWITCH_DIAG_SINGLE_CHECK 1
#endif

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
#ifdef SWITCH_DIAG_SD
    D_SD_REFERENCE_ERROR,
    D_SD_FLAGS,
#endif
#ifdef SWITCH_DIAG_T1
    D_T1_STOP,
    D_T1_TRANSFERS,
    D_T1_ID_CONFIG_RATE,
    D_T1_RAW,
    D_T1_CLOCKS,
#endif
#ifdef SWITCH_DIAG_T1R
    D_T1R_CLOCKS,
    D_T1R_SOURCE_I2C1_I2C5,
    D_T1R_PLLP_BASE_OUTA,
    D_T1R_PLLP_OUTB_I2C1,
#endif
#ifdef SWITCH_DIAG_T1V3
    D_T1V3_STOP,
    D_T1V3_WRITES_TRANSFERS,
    D_T1V3_DIVISOR,
    D_T1V3_TIMING,
    D_T1V3_ELAPSED,
    D_T1V3_PLLP_LOAD,
    D_T1V3_ID_STATUS,
#endif
#ifdef SWITCH_DIAG_T2
    D_T2_STOP,
    D_T2_WRITES_TRANSFERS,
    D_T2_DIVISOR,
    D_T2_ID_CONFIG_STATUS,
    D_T2_OLD_SAMPLE,
    D_T2_NEW_SAMPLE,
    D_T2_TIMES,
#endif
#ifdef SWITCH_DIAG_S2
    D_S2_STOP_READS,
    D_S2_CAR,
    D_S2_SDMMC1,
#endif
#ifdef SWITCH_DIAG_T4R
    D_T4R_STOP_READS,
    D_T4R_TMRUS,
    D_T4R_PTV_CONFIG,
    D_T4R_STATUS_STATE,
#endif
#ifdef SWITCH_DIAG_T3R
    D_T3R_STOP_READS,
    D_T3R_TMRUS,
    D_T3R_CAR,
    D_T3R_PWM,
    D_T3R_PINMUX_LCD_CAM1,
    D_T3R_PINMUX_SATA_USB,
    D_T3R_GPIO_A_CC_S,
    D_T3R_GPIO_S_V,
    D_T3R_PMC,
#endif
#ifdef SWITCH_DIAG_T3
    D_T3_STOP_STATE_WRITES,
    D_T3_WINDOW_IDLE,
    D_T3_WINDOW_RAIL,
    D_T3_WINDOW_SPINUP,
    D_T3_WINDOW_SPIN,
    D_T3_WINDOW_STOPPED,
    D_T3_READBACK_ON_PWM,
    D_T3_READBACK_ROUTE_OFF,
    D_T3_READBACK_RAIL_PAD,
#endif
#ifdef SWITCH_DIAG_T1T
    D_T1T_STOP_READS,
    D_T1T_CLOCKS_L,
    D_T1T_CLOCKS_H,
    D_T1T_SOURCE_I2C1_I2C5,
    D_T1T_PLLP_BASE,
    D_T1T_I2C1_TIMING,
    D_T1T_I2C5_TIMING,
#endif
};
#ifdef SWITCH_DIAG_T1T
_Static_assert(D_T1T_I2C5_TIMING < R_RECORD_WORDS, "T1-T results fit the probe record");
#endif
#ifdef SWITCH_DIAG_T1V3
_Static_assert(D_T1V3_ID_STATUS < R_RECORD_WORDS, "T1 v3 results fit the probe record");
#endif
#ifdef SWITCH_DIAG_T2
_Static_assert(D_T2_TIMES < R_RECORD_WORDS, "T2 results fit the probe record");
#endif
#ifdef SWITCH_DIAG_S2
_Static_assert(D_S2_SDMMC1 < R_RECORD_WORDS, "S2 results fit the probe record");
#endif
#ifdef SWITCH_DIAG_T4R
_Static_assert(D_T4R_STATUS_STATE < R_RECORD_WORDS, "T4-R results fit the probe record");
#endif
#ifdef SWITCH_DIAG_T3R
_Static_assert(D_T3R_PMC < R_RECORD_WORDS, "T3-R results fit the probe record");
#endif
#ifdef SWITCH_DIAG_T3
_Static_assert(D_T3_READBACK_RAIL_PAD < R_RECORD_WORDS, "T3 results fit the probe record");
#endif

/* 1280 x 720 as display_text draws it. The right column holds the registers, rows 0 to 30; the left column stops
 * before it; the status lines run full width under both. */
#define SCREEN_WIDTH     1280u
#define SCREEN_HEIGHT    720u
#define GLYPH_WIDTH      12u
#define GLYPH_HEIGHT     14u
#define ROW_HEIGHT       20u
#define TOP              12u
#define LEFT             16u
#define RIGHT            720u
#define FIRST_STATUS_ROW 31u
#define LAST_ROW         34u

_Static_assert(TOP + LAST_ROW * ROW_HEIGHT + GLYPH_HEIGHT <= SCREEN_HEIGHT, "the last row is on the screen");
_Static_assert(FIRST_STATUS_ROW >= SWITCH_DIAG_MC_COUNT + SWITCH_DIAG_GIC_COUNT, "status lines clear the registers");
_Static_assert(RIGHT + 36 * GLYPH_WIDTH <= SCREEN_WIDTH, "the longest register line fits");

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

static void put(unsigned column, unsigned row, struct switch_diag_line *line, uint32_t color)
{
    if (!diag.screen || row > LAST_ROW)
        return;
    switch_diag_line_clip(line, (SCREEN_WIDTH - column) / GLYPH_WIDTH);
#ifndef SWITCH_DIAG_SINGLE_CHECK
    if (column == LEFT && row < FIRST_STATUS_ROW)
        switch_diag_line_clip(line, (RIGHT - LEFT) / GLYPH_WIDTH);
#endif
    display_text(column, TOP + row * ROW_HEIGHT, line->text, color);
}

static void put_text(unsigned row, const char *text, uint32_t color)
{
    struct switch_diag_line line;
    switch_diag_line_start(&line, text);
    put(LEFT, row, &line, color);
}

static void say(const char *text, int bad)
{
    put_text(diag.status_row < LAST_ROW ? diag.status_row++ : LAST_ROW, text, bad ? DISPLAY_RED : DISPLAY_WHITE);
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
    if (!diag.screen) {
        r[R_STAGE] = 0xfd;
        probe_halt();
    }
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
    put_text(5, el_ok ? "EL1 STATE PASS" : "EL1 STATE FAILED. NO DEVICE READS", el_ok ? DISPLAY_GREEN : DISPLAY_RED);
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

#ifndef SWITCH_DIAG_SINGLE_CHECK
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

#endif

#ifdef SWITCH_DIAG_RESERVATIONS
/* Rows 16 to 30 of the left column: the banks and every reserved range as the walk saved them (reserve map entries
 * first, then /reserved-memory ranges), then the two areas D1 would use, each against the ranges and the banks. */
static void report_range(unsigned row, const char *label, uint64_t start, uint64_t end, uint32_t color)
{
    struct switch_diag_line line;
    switch_diag_range_line(&line, label, start, end);
    put(LEFT, row, &line, color);
}

static void report_area(unsigned row, const char *label, uint64_t start, uint64_t end,
                        const struct loader_memory_report *memory)
{
    struct switch_diag_line line;
    switch_diag_line_start(&line, label);
    switch_diag_line_add(&line, " ");
    switch_diag_line_hex(&line, start, SWITCH_DIAG_ADDRESS_DIGITS);
    switch_diag_line_add(&line, " ");
    switch_diag_line_hex(&line, end, SWITCH_DIAG_ADDRESS_DIGITS);
    const int over = switch_diag_first_overlap(&memory->ledger, start, end);
    const int bank = switch_diag_bank_holding(&memory->ledger, start, end);
    if (over >= 0) {
        switch_diag_line_add(&line, over < (int)memory->reservations ? " OVERLAPS MAP " : " OVERLAPS NODE RANGE ");
        switch_diag_line_decimal(&line, (uint64_t)(over < (int)memory->reservations ? over : over - (int)memory->reservations));
    } else if (bank < 0) {
        switch_diag_line_add(&line, " OUTSIDE THE BANKS");
    } else {
#ifdef SWITCH_DIAG_SD
        switch_diag_line_add(&line, " CLEAR IN BANK ");
#else
        switch_diag_line_add(&line, " CLEAR, BANK ");
#endif
        switch_diag_line_decimal(&line, (uint64_t)bank);
    }
    put(LEFT, row, &line, over >= 0 || bank < 0 ? DISPLAY_RED : DISPLAY_GREEN);
}

static void report_reservations(const struct loader_memory_report *memory)
{
    enum { FIRST_ROW = 16, MAX_SHOWN = 8 };
    unsigned row = FIRST_ROW;
    put_text(row++, "BANK AND RESERVED RANGES: START SIZE END", DISPLAY_WHITE);
    for (unsigned i = 0; i < 2; ++i)
        report_range(row++, i ? "BANK 1" : "BANK 0", memory->ledger.banks[i].start, memory->ledger.banks[i].end,
                     DISPLAY_WHITE);
    const unsigned count = memory->ledger.count < SWITCH_RAM_RANGES ? memory->ledger.count : SWITCH_RAM_RANGES;
    for (unsigned i = 0; i < count && i < MAX_SHOWN; ++i) {
        struct switch_diag_line label;
        const int map = i < memory->reservations;
        switch_diag_line_start(&label, map ? "MAP " : "NODE ");
        switch_diag_line_decimal(&label, map ? i : i - memory->reservations);
        report_range(row++, label.text, memory->ledger.reserved[i].start, memory->ledger.reserved[i].end,
                     DISPLAY_WHITE);
    }
    if (count > MAX_SHOWN) {
        struct switch_diag_line line;
        switch_diag_line_start(&line, "AND ");
        switch_diag_line_decimal(&line, count - MAX_SHOWN);
        switch_diag_line_add(&line, " MORE NOT SHOWN");
        put(LEFT, row++, &line, DISPLAY_RED);
    }
    report_area(28, "EXPORTER", SWITCH_DIAG_EXPORTER_START, SWITCH_DIAG_EXPORTER_END, memory);
    report_area(29, "SD REFERENCE", SWITCH_DIAG_SD_REFERENCE, SWITCH_DIAG_SD_REFERENCE + 0x1000, memory);
}
#endif

#ifdef SWITCH_DIAG_SD
/* Rows 23 to 27 and 30 of the left column, between D0-R's ranges and the status lines. */
static void report_sd(void)
{
    static const unsigned rows[SWITCH_DIAG_SD_LINES] = {23, 24, 25, 26, 27, 30};
    struct switch_diag_sd_result result;
    switch_diag_sd_evaluate_fixed(&result);
    for (unsigned i = 0; i < SWITCH_DIAG_SD_LINES; ++i)
        if (result.line[i].text.length)
            put(LEFT, rows[i], &result.line[i].text, result.line[i].bad ? DISPLAY_RED : DISPLAY_GREEN);
    probe_record[D_SD_REFERENCE_ERROR] = (uint64_t)(int64_t)result.reference_error;
    probe_record[D_SD_FLAGS] = result.flags_set;
}
#endif

#ifndef SWITCH_DIAG_SINGLE_CHECK
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
                                        "KERNEL DESTINATION", "BOOT ARGS PAGE"
#ifdef SWITCH_DIAG_RESERVATIONS
                                        ,
                                        "EXPORTER RUN AREA"
#endif
    };
#ifdef SWITCH_DIAG_RESERVATIONS
    enum { RANGE_COUNT = 6 };
#else
    enum { RANGE_COUNT = 5 };
#endif
    struct loader_memory_range ranges[RANGE_COUNT];
    ranges[0] = (struct loader_memory_range){UINT64_C(0xa0000000), (uint64_t)(uintptr_t)__image_end};
    ranges[1] = (struct loader_memory_range){SWITCH_DIAG_SD_REFERENCE, SWITCH_DIAG_SD_REFERENCE + 0x1000};
    ranges[2] = (struct loader_memory_range){SWITCH_DIAG_STAGE_START, SWITCH_DIAG_STAGE_END};
    ranges[3] = (struct loader_memory_range){SWITCH_DIAG_KERNEL_START, SWITCH_DIAG_KERNEL_END};
    ranges[4] = (struct loader_memory_range){SWITCH_DIAG_BOOT_ARGS, SWITCH_DIAG_BOOT_ARGS + 0x1000};
#ifdef SWITCH_DIAG_RESERVATIONS
    ranges[5] = (struct loader_memory_range){SWITCH_DIAG_EXPORTER_START, SWITCH_DIAG_EXPORTER_END};
#endif
    struct loader_memory_report memory;
    unsigned index = 0;
    const int walk = loader_check_fdt_memory_ranges((const uint8_t *)(uintptr_t)probe_record[R_FDT],
                                                    (uint32_t)SWITCH_DIAG_FDT_BYTES, ranges, RANGE_COUNT, &memory, &index);
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
#ifdef SWITCH_DIAG_RESERVATIONS
    report_reservations(&memory);
#endif
}

#endif

#ifdef SWITCH_DIAG_T1R
static void report_t1r(void)
{
    struct switch_t1r_result result;
    switch_t1r_run_on_hardware(&result);
    struct switch_t1r_line lines[SWITCH_T1R_LINES];
    switch_t1r_lines(&result, lines);
    for (unsigned i = 0; i < SWITCH_T1R_LINES; ++i)
        if (lines[i].text.length)
            put(LEFT, 9 + i, &lines[i].text, lines[i].bad ? DISPLAY_RED : DISPLAY_WHITE);
    probe_record[D_T1R_CLOCKS] = (uint64_t)result.rst_devices_l << 32 | result.clk_out_enb_l;
    probe_record[D_T1R_SOURCE_I2C1_I2C5] = (uint64_t)result.source[0] << 32 | result.source[4];
    probe_record[D_T1R_PLLP_BASE_OUTA] = (uint64_t)result.pllp_base << 32 | result.pllp_outa;
    probe_record[D_T1R_PLLP_OUTB_I2C1] = (uint64_t)result.pllp_outb << 32 | (uint32_t)result.i2c1;
}
#endif

#ifdef SWITCH_DIAG_T1T
static void report_t1t(void)
{
    struct switch_t1t_result result;
    switch_t1t_run_on_hardware(&result);
    struct switch_t1t_line lines[SWITCH_T1T_LINES];
    switch_t1t_lines(&result, lines);
    for (unsigned i = 0; i < SWITCH_T1T_LINES; ++i)
        if (lines[i].text.length)
            put(LEFT, 9 + i, &lines[i].text, lines[i].bad ? DISPLAY_RED : DISPLAY_WHITE);
    probe_record[D_T1T_STOP_READS] = (uint64_t)result.stop << 32 | result.reads;
    probe_record[D_T1T_CLOCKS_L] = (uint64_t)result.rst_devices_l << 32 | result.clk_out_enb_l;
    probe_record[D_T1T_CLOCKS_H] = (uint64_t)result.rst_devices_h << 32 | result.clk_out_enb_h;
    probe_record[D_T1T_SOURCE_I2C1_I2C5] = (uint64_t)result.source[0] << 32 | result.source[4];
    probe_record[D_T1T_PLLP_BASE] = result.pllp_base;
    probe_record[D_T1T_I2C1_TIMING] = (uint64_t)result.block[0].word[T1T_TIMING_0] << 32 | result.block[0].word[T1T_TIMING_1];
    probe_record[D_T1T_I2C5_TIMING] = (uint64_t)result.block[1].word[T1T_TIMING_0] << 32 | result.block[1].word[T1T_TIMING_1];
}
#endif

#ifdef SWITCH_DIAG_T1V3
static void report_t1v3(void)
{
    struct switch_t1v3_result result;
    switch_t1v3_run_on_hardware(&result);
    struct switch_t1v3_line lines[SWITCH_T1V3_LINES];
    switch_t1v3_lines(&result, lines);
    for (unsigned i = 0; i < SWITCH_T1V3_LINES; ++i)
        if (lines[i].text.length)
            put(LEFT, 9 + i, &lines[i].text, lines[i].bad ? DISPLAY_RED : DISPLAY_WHITE);
    probe_record[D_T1V3_STOP] = (uint64_t)result.stop;
    probe_record[D_T1V3_WRITES_TRANSFERS] = (uint64_t)result.writes << 32 | result.transfers;
    probe_record[D_T1V3_DIVISOR] = (uint64_t)result.divisor << 32 | result.divisor_after;
    probe_record[D_T1V3_TIMING] = (uint64_t)result.timing_0 << 32 | result.timing_1;
    probe_record[D_T1V3_ELAPSED] = (uint64_t)result.transaction[0].elapsed_us << 32 | result.transaction[1].elapsed_us;
    probe_record[D_T1V3_PLLP_LOAD] = (uint64_t)result.pllp_base << 32 | result.load_after;
    probe_record[D_T1V3_ID_STATUS] = (uint64_t)result.have_id << 40 | (uint64_t)result.id << 32 | result.status_after;
}
#endif

#ifdef SWITCH_DIAG_T2
static void report_t2(void)
{
    struct switch_t2_image_result result;
    switch_t2_run_on_hardware(&result);
    struct switch_t2_line lines[SWITCH_T2_LINES];
    switch_t2_lines(&result, lines);
    for (unsigned i = 0; i < SWITCH_T2_LINES; ++i)
        if (lines[i].text.length)
            put(LEFT, 9 + i, &lines[i].text, lines[i].bad ? DISPLAY_RED : DISPLAY_WHITE);
    const struct switch_t2_result *s = &result.sensor;
    probe_record[D_T2_STOP] = (uint64_t)result.stop;
    probe_record[D_T2_WRITES_TRANSFERS] = (uint64_t)result.writes << 32 | result.transfers;
    probe_record[D_T2_DIVISOR] = (uint64_t)result.probe.divisor << 32 | result.probe.divisor_after;
    probe_record[D_T2_ID_CONFIG_STATUS] = (uint64_t)s->id << 24 | (uint64_t)s->config_before << 16 | (uint64_t)s->status_before << 8 |
                                          s->status_last;
    probe_record[D_T2_OLD_SAMPLE] = (uint64_t)s->old_sample.remote_high << 24 | (uint64_t)s->old_sample.remote_low << 16 |
                                    (uint64_t)s->old_sample.local_high << 8 | s->old_sample.local_low;
    probe_record[D_T2_NEW_SAMPLE] = (uint64_t)s->new_sample.remote_high << 24 | (uint64_t)s->new_sample.remote_low << 16 |
                                    (uint64_t)s->new_sample.local_high << 8 | s->new_sample.local_low;
    probe_record[D_T2_TIMES] = (uint64_t)s->busy_first_us << 32 | s->done_us;
}
#endif

#ifdef SWITCH_DIAG_S2
/* A step is on the screen only when it was drawn and the settle ran; with no screen nothing may be read. */
static int show_s2(void *context, unsigned step, const struct switch_s2_result *result)
{
    (void)context;
    if (!diag.screen)
        return 0;
    struct switch_s2_line line;
    switch_s2_screen_line(step, result, &line);
    put(LEFT, 9 + step, &line.text, line.bad ? DISPLAY_RED : DISPLAY_WHITE);
    return switch_s2_settle();
}

static void report_s2(void)
{
    struct switch_s2_result result;
    switch_s2_run_on_hardware(&result, show_s2);
    probe_record[D_S2_STOP_READS] = (uint64_t)result.stop << 32 | result.reads;
    probe_record[D_S2_CAR] = (uint64_t)result.rst_devices_l << 32 | result.clk_out_enb_l;
    probe_record[D_S2_SDMMC1] = (uint64_t)result.sd_present_state << 32 | result.sd_clock_control;
}
#endif

#ifdef SWITCH_DIAG_T4R
static int show_t4r(void *context, unsigned step, const struct switch_t4r_result *result)
{
    (void)context;
    if (!diag.screen)
        return 0;
    struct switch_t4r_line line;
    switch_t4r_screen_line(step, result, &line);
    put(LEFT, 9 + step, &line.text, line.bad ? DISPLAY_RED : DISPLAY_WHITE);
    return switch_t4r_settle();
}

static void report_t4r(void)
{
    put_text(24, "A READ THAT HANGS THE BUS IS NOT LEFT BY ANY TIMEOUT HERE. IF THE SCREEN STOPS, HOLD POWER", DISPLAY_WHITE);
    put_text(25, "THE STATE SHOWN IS AFTER HEKATE'S INIT. IT IS NOT THE STATE RIGHT AFTER A RESET", DISPLAY_WHITE);
    put_text(26, "TMRUS AND A RETURN TO HEKATE ARE NOT EVIDENCE OF A WATCHDOG RESET. NO WATCHDOG IS SET OR STOPPED", DISPLAY_WHITE);
    struct switch_t4r_result result;
    switch_t4r_run_on_hardware(&result, show_t4r);
    probe_record[D_T4R_STOP_READS] = (uint64_t)result.stop << 32 | result.reads;
    probe_record[D_T4R_TMRUS] = (uint64_t)result.tmrus_first << 32 | result.tmrus_second;
    probe_record[D_T4R_PTV_CONFIG] = (uint64_t)result.ptv << 32 | result.config;
    probe_record[D_T4R_STATUS_STATE] = (uint64_t)result.status << 32 | result.state << 8 | result.why;
}
#endif

#ifdef SWITCH_DIAG_T3R
/* The right column of the 22 read cells starts here; a cell is SWITCH_T3R_CELL characters wide. */
#define T3R_RIGHT_COLUMN 656u
_Static_assert(LEFT + SWITCH_T3R_CELL * GLYPH_WIDTH <= T3R_RIGHT_COLUMN, "the cells do not overlap");
_Static_assert(T3R_RIGHT_COLUMN + SWITCH_T3R_CELL * GLYPH_WIDTH <= SCREEN_WIDTH, "the right column is on the screen");
_Static_assert(9 + SWITCH_T3R_SUMMARY_FIRST_ROW + 5 < 26, "the summary rows end above the notes");
_Static_assert(LEFT + SWITCH_DIAG_LINE_MAX * GLYPH_WIDTH <= SCREEN_WIDTH, "a full width line is on the screen");

static int show_t3r(void *context, unsigned step, const struct switch_t3r_result *result)
{
    (void)context;
    if (!diag.screen)
        return 0;
    struct switch_t3r_line line;
    switch_t3r_screen_line(step, result, &line);
    put(line.right ? T3R_RIGHT_COLUMN : LEFT, 9 + line.row, &line.text, line.bad ? DISPLAY_RED : DISPLAY_WHITE);
    return switch_t3r_settle();
}

static uint64_t t3r_low_bytes(const struct switch_t3r_result *r, const unsigned regs[8])
{
    uint64_t packed = 0;
    for (unsigned i = 0; i < 8; ++i)
        packed |= (uint64_t)(r->value[regs[i]] & 0xffu) << (8 * i);
    return packed;
}

static void report_t3r(void)
{
    put_text(26, "PERSON: NO NEW LINE FOR 30 S, HOLD POWER. THE READS END BY THEMSELVES AFTER 20 S. THEN 15 S AND A RESET", DISPLAY_WHITE);
    put_text(27, "A READ THAT HANGS THE BUS IS NOT LEFT BY ANY TIMEOUT HERE. IF THE SCREEN STOPS, HOLD POWER", DISPLAY_WHITE);
    put_text(28, "STATE SHOWN IS AFTER HEKATE AND U-BOOT. EXPECTED ASSUMES NYX RAN THE FAN OFF. A DIFFERENCE IS NOT A FAULT", DISPLAY_WHITE);
    put_text(29, "PMC AND GPIO READS AT NON-SECURE EL1 ARE UNTESTED. THE LINE ON SCREEN WHEN IT STOPS NAMES THE REGISTER", DISPLAY_WHITE);
    struct switch_t3r_result result;
    switch_t3r_run_on_hardware(&result, show_t3r);
    static const unsigned gpio_a_cc_s[8] = {T3R_GPIO_A_CNF,  T3R_GPIO_A_OE, T3R_GPIO_A_OUT, T3R_GPIO_CC_CNF,
                                            T3R_GPIO_CC_OE, T3R_GPIO_CC_OUT, T3R_GPIO_S_CNF, T3R_GPIO_S_OE};
    probe_record[D_T3R_STOP_READS] = (uint64_t)result.stop << 32 | result.reads;
    probe_record[D_T3R_TMRUS] = (uint64_t)result.value[T3R_TMRUS] << 32 | result.tmrus_second;
    probe_record[D_T3R_CAR] = (uint64_t)result.value[T3R_CAR_RST_DEVICES_L] << 32 | result.value[T3R_CAR_CLK_OUT_ENB_L];
    probe_record[D_T3R_PWM] = (uint64_t)result.value[T3R_PWM_CSR_1] << 32 | result.gate << 16 | result.pwm << 8 | result.pwm_why;
    probe_record[D_T3R_PINMUX_LCD_CAM1] = (uint64_t)result.value[T3R_PINMUX_LCD_GPIO2] << 32 | result.value[T3R_PINMUX_CAM1_PWDN];
    probe_record[D_T3R_PINMUX_SATA_USB] =
        (uint64_t)result.value[T3R_PINMUX_SATA_LED_ACTIVE] << 32 | result.value[T3R_PINMUX_USB_VBUS_EN0];
    probe_record[D_T3R_GPIO_A_CC_S] = t3r_low_bytes(&result, gpio_a_cc_s);
    probe_record[D_T3R_GPIO_S_V] = (result.value[T3R_GPIO_S_IN] & 0xffu) | (result.value[T3R_GPIO_V_CNF] & 0xffu) << 8 |
                                   (result.value[T3R_GPIO_V_OE] & 0xffu) << 16;
    probe_record[D_T3R_PMC] = (uint64_t)result.value[T3R_PMC_NO_IOPOWER] << 32 | result.value[T3R_PMC_PWR_DET_VAL];
}
#endif

#ifdef SWITCH_DIAG_T3
_Static_assert(9 + SWITCH_T3_LINE_ROWS < 26, "the T3 lines end above the notes");
_Static_assert(LEFT + SWITCH_DIAG_LINE_MAX * GLYPH_WIDTH <= SCREEN_WIDTH, "a full width line is on the screen");

static int show_t3(void *context, unsigned step, const struct switch_t3_result *result)
{
    (void)context;
    if (!diag.screen)
        return 0;
    struct switch_t3_line line;
    switch_t3_screen_line(step, result, &line);
    put(LEFT, 9 + line.row, &line.text, line.bad ? DISPLAY_RED : DISPLAY_WHITE);
    return switch_t3_settle();
}

static uint64_t t3_window_word(const struct switch_t3_window *w)
{
    return (uint64_t)w->samples << 32 | (uint64_t)(w->transitions & 0xffffffu) << 8 | w->first_level << 4 | w->last_level << 1 |
           (w->cut != 0);
}

static void report_t3(void)
{
    put_text(26, "PERSON: NO NEW LINE FOR 30 S: HOLD POWER. THE RAIL IS ON FOR 15 S AT MOST. THEN 15 S AND A RESET", DISPLAY_WHITE);
    put_text(27, "PLUGGED INTO THE RAIL: NOTHING BUT THE FAN. NO JOY-CON AND NO USB. THE OFF STEPS RUN BEFORE THE RESET", DISPLAY_WHITE);
    put_text(28, "A RESET IS NOT CLAIMED TO RESTORE ANY REGISTER. THE OFF STEPS ARE THE ONLY RESTORE AND EACH IS READ BACK", DISPLAY_WHITE);
    put_text(29, "PASS NEEDS MORE TRANSITIONS AT DUTY 51 THAN IN THE TWO WINDOWS BEFORE. IT IS NOT AN RPM", DISPLAY_WHITE);
    struct switch_t3_result result;
    switch_t3_run_on_hardware(&result, show_t3);
    probe_record[D_T3_STOP_STATE_WRITES] = (uint64_t)result.stop << 48 | (uint64_t)result.state << 40 |
                                           (uint64_t)result.issued << 32 | result.mismatched << 24 | result.writes << 16 | result.reads;
    probe_record[D_T3_WINDOW_IDLE] = t3_window_word(&result.window[T3_WINDOW_IDLE]);
    probe_record[D_T3_WINDOW_RAIL] = t3_window_word(&result.window[T3_WINDOW_RAIL]);
    probe_record[D_T3_WINDOW_SPINUP] = t3_window_word(&result.window[T3_WINDOW_SPINUP]);
    probe_record[D_T3_WINDOW_SPIN] = t3_window_word(&result.window[T3_WINDOW_SPIN]);
    probe_record[D_T3_WINDOW_STOPPED] = t3_window_word(&result.window[T3_WINDOW_STOPPED]);
    probe_record[D_T3_READBACK_ON_PWM] = (uint64_t)result.readback[T3_W_RAIL_ON] << 32 | result.readback[T3_W_PWM_DUTY];
    probe_record[D_T3_READBACK_ROUTE_OFF] = (uint64_t)result.readback[T3_W_ROUTE_PWM] << 32 | result.readback[T3_O_PWM_OFF];
    probe_record[D_T3_READBACK_RAIL_PAD] = (uint64_t)result.readback[T3_O_RAIL_OFF] << 32 | result.readback[T3_O_PARK_PAD];
}
#endif

#ifdef SWITCH_DIAG_T1
/* Rows 9 to 16, full width: T1 draws no register column. */
static void report_t1(void)
{
    struct switch_t1_result result;
    switch_t1_run_on_hardware(&result);
    struct switch_t1_line lines[SWITCH_T1_LINES];
    switch_t1_lines(&result, lines);
    for (unsigned i = 0; i < SWITCH_T1_LINES; ++i)
        if (lines[i].text.length)
            put(LEFT, 9 + i, &lines[i].text, lines[i].bad ? DISPLAY_RED : DISPLAY_WHITE);
    probe_record[D_T1_STOP] = (uint64_t)result.stop;
    probe_record[D_T1_TRANSFERS] = result.transfers;
    probe_record[D_T1_ID_CONFIG_RATE] = (uint64_t)result.value[0] << 16 | (uint64_t)result.value[1] << 8 | result.value[2];
    probe_record[D_T1_RAW] = (uint64_t)result.value[3] << 8 | result.value[4];
    probe_record[D_T1_CLOCKS] = (uint64_t)result.rst_devices_l << 32 | result.clk_out_enb_l;
}
#endif

void switch_diag_after_transition(void)
{
    volatile uint64_t *r = probe_record;
    const struct switch_diag_el1 el1 = {r[R_EL1_EL],    r[R_EL1_SCTLR],     r[R_EL1_DAIF],     r[R_EL1_CNTFRQ],
                                        r[R_HCR_AFTER], r[R_CNTHCTL_AFTER], r[R_CNTVOFF_AFTER]};
    const int el_ok = !switch_diag_el1_state(&el1);
    begin_screen();
#ifdef SWITCH_DIAG_T1
    put_text(0, "LOAD DIAG T1. I2C1 AND CAR READS. TMP451 POINTER WRITES ONLY. NO FAN. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_T1V3)
    put_text(0, "LOAD DIAG T1V3. I2C1 DIVISOR WRITE AND ONE TMP451 ID READ. NO CAR WRITE. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_T2)
    put_text(0, "LOAD DIAG T2. I2C1 DIVISOR WRITE AND ONE TMP451 ONE-SHOT CONVERSION. NO CAR WRITE. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_T4R)
    put_text(0, "LOAD DIAG T4R. FOUR TKE REGISTER READS ONLY. EACH READ SHOWN FIRST. NO WRITE. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_T3R)
    put_text(0, "LOAD DIAG T3R. FAN STATE READS ONLY. EACH READ SHOWN FIRST. NO WRITE. NO FAN. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_T3)
    put_text(0, "LOAD DIAG T3. FAN ON AT DUTY 51 FOR A FEW SECONDS. RAIL A5 HIGH. NO KERNEL. NO BOOT", DISPLAY_RED);
#elif defined(SWITCH_DIAG_S2)
    put_text(0, "LOAD DIAG S2. CAR AND SDMMC1 REGISTER READS ONLY. EACH READ SHOWN FIRST. NO WRITE. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_T1T)
    put_text(0, "LOAD DIAG T1T. CAR AND I2C1/I2C5 REGISTER READS ONLY. NO I2C TRANSFER. NO WRITE. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_T1R)
    put_text(0, "LOAD DIAG T1R. CAR AND I2C1 READS ONLY. NO I2C TRANSFER. NO WRITE. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_SD)
    put_text(0, "LOAD DIAG D1. READ ONLY. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#elif defined(SWITCH_DIAG_RESERVATIONS)
    put_text(0, "LOAD DIAG D0-R. READ ONLY. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#else
    put_text(0, "LOAD DIAG D0. READ ONLY. NO KERNEL. NO BOOT", DISPLAY_GREEN);
#endif
    report_el1(&el1, el_ok);
    report_cpu();
    if (el_ok) {
        const uint32_t hidrev = switch_diag_read32(0x70000804), odm4 = switch_diag_read32(0x7000f9d8);
        const struct switch_board *board = switch_board_detect(hidrev, odm4);
        const int known = board->t210b01 != 0xff;
        r[D_BOARD] = known ? (uint64_t)board->t210b01 << 8 | board->sku : UINT64_MAX;
        report_board(board, hidrev, odm4);
#ifdef SWITCH_DIAG_T1
        if (known && board->t210b01 == 0)
            report_t1();
        else
            put_text(9, "NOT AN ERISTA. NO T1 REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_T1V3)
        if (known && board->t210b01 == 0)
            report_t1v3();
        else
            put_text(9, "NOT AN ERISTA. NO T1V3 REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_T2)
        if (known && board->t210b01 == 0)
            report_t2();
        else
            put_text(9, "NOT AN ERISTA. NO T2 REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_T4R)
        if (known && board->t210b01 == 0)
            report_t4r();
        else
            put_text(9, "NOT AN ERISTA. NO T4R REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_T3R)
        if (known && board->t210b01 == 0)
            report_t3r();
        else
            put_text(9, "NOT AN ERISTA. NO T3R REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_T3)
        if (known && board->t210b01 == 0)
            report_t3();
        else
            put_text(9, "NOT AN ERISTA. NO T3 REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_S2)
        if (known && board->t210b01 == 0)
            report_s2();
        else
            put_text(9, "NOT AN ERISTA. NO S2 REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_T1T)
        if (known && board->t210b01 == 0)
            report_t1t();
        else
            put_text(9, "NOT AN ERISTA. NO T1T REGISTER TOUCHED", DISPLAY_RED);
#elif defined(SWITCH_DIAG_T1R)
        if (known && board->t210b01 == 0)
            report_t1r();
        else
            put_text(9, "NOT AN ERISTA. NO T1R REGISTER TOUCHED", DISPLAY_RED);
#else
        if (known)
            report_memory_controller();
        else
            put_text(8, "UNKNOWN BOARD. MC AND GIC READS SKIPPED", DISPLAY_RED);
#endif
    } else {
        put_text(1, "NO DEVICE REGISTER WAS READ", DISPLAY_RED);
    }
#ifndef SWITCH_DIAG_SINGLE_CHECK
    report_fdt();
#endif
#ifdef SWITCH_DIAG_SD
    report_sd();
#endif
#ifdef SWITCH_DIAG_T1
    put_text(18, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_T1V3)
    put_text(22, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_T2)
    put_text(24, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_T4R)
    put_text(28, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_T3R)
    put_text(30, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_T3)
    put_text(30, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_S2)
    put_text(22, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_T1T)
    put_text(26, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#elif defined(SWITCH_DIAG_T1R)
    put_text(21, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD. PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#else
    put_text(14, "BOOT STAYS 0. NO KERNEL WINDOW IN THIS BUILD", DISPLAY_WHITE);
    put_text(15, "PHOTOGRAPH THE SCREEN", DISPLAY_WHITE);
#endif
    r[R_STAGE] = 0xd0;
    switch_diag_exit(say);
}

void probe_show_exception(uint64_t el, uint64_t esr, uint64_t far, uint64_t pc)
{
    probe_record[R_STAGE] = 0xee;
    if (diag.screen) {
        hex_line(LEFT, 17, "EXCEPTION AT EL ", el >> 2, 1, "", DISPLAY_RED);
        hex_line(LEFT, 18, "ESR ", esr, 8, "", DISPLAY_RED);
        hex_line(LEFT, 19, "FAR ", far, 16, "", DISPLAY_RED);
        hex_line(LEFT, 20, "PC ", pc, 16, "", DISPLAY_RED);
    }
    if (el != 4)
        probe_halt();
    switch_diag_exit(say);
}
