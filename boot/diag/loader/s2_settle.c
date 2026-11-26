#include <stdint.h>
#include "s2.h"

/* More iterations than the wait needs on any clock this core runs at; a counter that stopped ends the wait by count. */
#define SETTLE_ITERATION_LIMIT 200000000u

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

/* The loader runs with the MMU off, so the framebuffer stores are Device writes and need no cache maintenance. The barrier
 * orders them before the wait; the wait covers the display's own scan-out, which the CPU cannot observe. */
int switch_s2_settle(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
    const uint64_t ticks = switch_s2_settle_ticks(frequency());
    if (!ticks)
        return 0;
    const uint64_t start = counter();
    for (uint32_t i = 0; i < SETTLE_ITERATION_LIMIT; ++i)
        if (counter() - start >= ticks)
            return 1;
    return 0;
}
