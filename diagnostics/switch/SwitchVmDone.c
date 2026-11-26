/* The first-XNU observation's end (ERISTA_FIRST_XNU_BOOT.md): once arm_vm_init has returned in a trace build, say so
 * on the screen and, when asked, stop before IOKit and the SD driver exist. Debug kernels only.
 */
#include <stdint.h>
#include <stddef.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "SwitchDiag.h"
#include "SwitchDiagDisplay.h"
#include "SwitchDiagHud.h"
#include "SwitchVmReset.h"

#if DEVELOPMENT || DEBUG
extern unsigned switch_diag_vm_stage21;

static void band(volatile uint32_t *pixels, unsigned first, unsigned last)
{
    for (unsigned y = first; y < last; ++y) {
        for (unsigned x = 0; x < 1280; ++x)
            switch_diag_pixel(pixels, x, y, SWITCH_BACKGROUND);
    }
}

/* Parks the CPU with every interrupt masked. Nothing resets or powers the console off from here: only the power button ends it. */
static void park(void)
{
    for (;;)
        __asm__ volatile("msr daifset, #15\n\tdsb sy\n\twfi" ::: "memory");
}

static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(value)::"memory");
    return value;
}

static int counter_advances(void)
{
    const uint64_t first = counter();
    for (unsigned i = 0; i < 1000; ++i)
        if (counter() != first)
            return 1;
    return 0;
}

static void message(volatile uint32_t *pixels, const char *text, uint32_t colour)
{
    if (!pixels)
        return;
    band(pixels, 100, 150);
    (void)switch_diag_text(pixels, 16, 104, 704, text, 104, colour);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* The wait is the loader's (boot/diag/loader/exit.c): the generic timer's event stream, one event every 2^16 counts, so wfe wakes
 * to compare. If the stream never fires, wfe never returns; that is unresolved (ERISTA_PROTECTION.md). The call is made once. */
static void reset_after(volatile uint32_t *pixels, unsigned seconds)
{
    uint64_t current_el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    const uint64_t ticks = switch_vm_reset_ticks(frequency, seconds);
    if (current_el != 4 || !ticks || !counter_advances()) {
        message(pixels, "NOT EL1 OR COUNTER NOT AS EXPECTED. NO RESET. HOLD POWER", SWITCH_RED);
        park();
    }
    char text[64] = "RESETTING IN   S. HOLD POWER TO STOP";
    text[13] = seconds >= 10u ? (char)('0' + seconds / 10u) : ' ';
    text[14] = (char)('0' + seconds % 10u);
    message(pixels, text, SWITCH_RED);
    __asm__ volatile("msr daifset, #15" ::: "memory");
    uint64_t cntkctl;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
    /* EVNTEN, EVNTDIR clear, EVNTI 15; the PL0 access bits stay as the kernel set them. */
    cntkctl = (cntkctl & ~(uint64_t)0xfc) | 0xf4;
    __asm__ volatile("msr cntkctl_el1, %0\n\tisb" ::"r"(cntkctl) : "memory");
    const uint64_t deadline = counter() + ticks;
    while (switch_vm_reset_before(counter(), deadline))
        __asm__ volatile("wfe" ::: "memory");
    register uint64_t x0 __asm__("x0") = SWITCH_PSCI_SYSTEM_RESET;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("dsb sy\n\tsmc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                       "memory");
    char failed[64] = "SMC RETURNED 0000000000000000. NO RETRY. HOLD POWER";
    for (unsigned i = 0; i < 16; ++i)
        failed[13 + i] = "0123456789ABCDEF"[(x0 >> (60 - 4 * i)) & 0xf];
    message(pixels, failed, SWITCH_RED);
    park();
}

void switch_diag_vm_done(void)
{
    if (!switch_diag_vm_stage21)
        return;
    unsigned seconds = 0;
    const enum switch_vm_reset_arg arg = switch_vm_reset_parse(PE_boot_args(), &seconds);
    vm_offset_t mapped = ml_io_map((vm_offset_t)SWITCH_FB_PA, (vm_size_t)SWITCH_FB_BYTES);
    volatile uint32_t *pixels = mapped ? (volatile uint32_t *)mapped : 0;
    if (pixels) {
        band(pixels, 20, 150);
        (void)switch_diag_text(pixels, 16, 24, 704, "XNU VM INITIALIZED", 104, SWITCH_GREEN);
        (void)switch_diag_text(pixels, 16, 64, 704, "ARM_VM_INIT RETURNED AFTER STAGE 21", 104, SWITCH_WHITE);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    if (arg == SWITCH_VM_RESET_ABSENT)
        return;
    if (arg == SWITCH_VM_RESET_INVALID) {
        message(pixels, "BAD SWITCH_STOP_AFTER_VM. NO RESET. HOLD POWER", SWITCH_RED);
        park();
    }
    reset_after(pixels, seconds);
}
#endif
