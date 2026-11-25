#include <stdint.h>
#include "checks.h"
#include "diag.h"
#include "report.h"

extern void probe_halt(void) __attribute__((noreturn));

static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value)::"memory");
    return value;
}

static uint64_t frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
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

void switch_diag_exit(switch_diag_say_fn say)
{
    const uint64_t ticks = switch_diag_wait_ticks(frequency(), SWITCH_DIAG_WAIT_SECONDS);
    if (!ticks || !counter_advances()) {
        say("COUNTER FREQUENCY OR COUNT NOT AS EXPECTED. NO RESET. HOLD THE POWER BUTTON", 1);
        probe_halt();
    }
    say("RESET IN 15 S. PSCI SYSTEM_RESET FROM THE LOADER IS NOT VERIFIED", 0);
    const uint64_t deadline = counter() + ticks;
    /* EVNTEN, EVNTI 15: an event every 2^16 counts (3.4 ms), so wfe wakes to compare instead of the core spinning. */
    __asm__ volatile("msr cntkctl_el1, %0; isb" ::"r"((uint64_t)0xf4) : "memory");
    while (switch_diag_before(counter(), deadline))
        __asm__ volatile("wfe" ::: "memory");
    register uint64_t x0 __asm__("x0") = SWITCH_DIAG_PSCI_SYSTEM_RESET;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("dsb sy; smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                       "memory");
    struct switch_diag_line line;
    switch_diag_line_start(&line, "PSCI SYSTEM_RESET RETURNED ");
    switch_diag_line_hex(&line, x0, 16);
    switch_diag_line_add(&line, ". HOLD THE POWER BUTTON");
    say(line.text, 1);
    probe_halt();
}
