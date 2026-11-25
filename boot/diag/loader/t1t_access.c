#include <stdint.h>
#include "t1t.h"

/* The only code that uses a device address for T1-T. It loads and returns; there is no store counterpart. */
__attribute__((noinline)) uint32_t switch_t1t_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static uint32_t io_read(void *context, unsigned reg)
{
    (void)context;
    return switch_t1t_access(reg);
}

void switch_t1t_run_on_hardware(struct switch_t1t_result *result)
{
    static const struct switch_t1t_io io = {0, io_read};
    switch_t1t_run(&io, result);
}
