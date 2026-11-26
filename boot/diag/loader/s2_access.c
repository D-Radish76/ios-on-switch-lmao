#include <stdint.h>
#include "s2.h"

/* The only code that uses a device address for S2. Each one loads and returns; there is no store counterpart. */
__attribute__((noinline)) uint32_t switch_s2_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

__attribute__((noinline)) uint32_t switch_s2_read16(uint32_t address)
{
    return *(const volatile uint16_t *)(uintptr_t)address;
}

static uint32_t io_read(void *context, unsigned reg, unsigned width)
{
    (void)context;
    return switch_s2_access(reg, width);
}

void switch_s2_run_on_hardware(struct switch_s2_result *result, switch_s2_show_fn show)
{
    const struct switch_s2_io io = {0, io_read, show};
    switch_s2_run(&io, result);
}
