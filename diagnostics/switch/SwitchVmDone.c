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

#if DEVELOPMENT || DEBUG
extern unsigned switch_diag_vm_stage21;

static void band(volatile uint32_t *pixels, unsigned first, unsigned last)
{
    for (unsigned y = first; y < last; ++y) {
        for (unsigned x = 0; x < 1280; ++x)
            switch_diag_pixel(pixels, x, y, SWITCH_BACKGROUND);
    }
}

/* Parks the CPU with every interrupt masked. This is not a safe stop: nothing resets or powers the console off, and
 * PSCI is not reachable on this board's ADT (ERISTA_FIRST_XNU_BOOT.md). Only the power button ends it. */
static void park(void)
{
    for (;;)
        __asm__ volatile("msr daifset, #15\n\tdsb sy\n\twfi" ::: "memory");
}

void switch_diag_vm_done(void)
{
    if (!switch_diag_vm_stage21)
        return;
    unsigned stop = 0;
    (void)PE_parse_boot_argn("switch_stop_after_vm", &stop, sizeof(stop));
    vm_offset_t mapped = ml_io_map((vm_offset_t)SWITCH_FB_PA, (vm_size_t)SWITCH_FB_BYTES);
    if (mapped) {
        volatile uint32_t *pixels = (volatile uint32_t *)mapped;
        band(pixels, 20, 150);
        (void)switch_diag_text(pixels, 16, 24, 704, "XNU VM INITIALIZED", 104, SWITCH_GREEN);
        (void)switch_diag_text(pixels, 16, 64, 704, "ARM_VM_INIT RETURNED AFTER STAGE 21", 104, SWITCH_WHITE);
        if (stop)
            (void)switch_diag_text(pixels, 16, 104, 704, "STOPPED. HOLD POWER TO END", 104, SWITCH_RED);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    if (stop)
        park();
}
#endif
