#include <stdint.h>
#include "t3r.h"

/* The only code that uses a device address for T3-R. It loads and returns; there is no store counterpart. */
__attribute__((noinline)) uint32_t switch_t3r_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static uint32_t io_read(void *context, unsigned reg)
{
    (void)context;
    return switch_t3r_access(reg);
}

static int io_expired(void *context)
{
    return switch_t3r_budget_spent(*(const uint64_t *)context);
}

void switch_t3r_run_on_hardware(struct switch_t3r_result *result, switch_t3r_show_fn show)
{
    uint64_t start = switch_t3r_counter();
    const struct switch_t3r_io io = {&start, io_read, show, io_expired};
    switch_t3r_run(&io, result);
}
