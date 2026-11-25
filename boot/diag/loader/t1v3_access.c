#include <stdint.h>
#include "t1v3.h"

/* The only code that uses a device address for T1 v3. The first loads, the second stores, and both return. */
__attribute__((noinline)) uint32_t switch_t1v3_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

__attribute__((noinline)) void switch_t1v3_write32(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static uint32_t io_read(void *context, unsigned reg)
{
    (void)context;
    return switch_t1v3_access(reg, 0, 0);
}

static void io_write(void *context, unsigned reg, uint32_t value)
{
    (void)context;
    switch_t1v3_access(reg, 1, value);
}

static uint64_t io_now_us(void *context)
{
    (void)context;
    uint64_t count;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(count)::"memory");
    return count * 5u / 96u; /* 19.2 MHz, which the entry check has already required */
}

void switch_t1v3_run_on_hardware(struct switch_t1v3_result *result)
{
    static const struct switch_t1v3_io io = {0, io_read, io_write, io_now_us};
    switch_t1v3_run(&io, result);
}
