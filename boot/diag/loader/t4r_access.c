#include <stdint.h>
#include "t4r.h"

/* The only code that uses a device address for T4-R. It loads and returns; there is no store counterpart. */
__attribute__((noinline)) uint32_t switch_t4r_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static uint32_t io_read(void *context, unsigned reg)
{
    (void)context;
    return switch_t4r_access(reg);
}

void switch_t4r_run_on_hardware(struct switch_t4r_result *result, switch_t4r_show_fn show)
{
    const struct switch_t4r_io io = {0, io_read, show};
    switch_t4r_run(&io, result);
}
