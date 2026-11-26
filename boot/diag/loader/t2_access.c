#include <stdint.h>
#include "t2_image.h"

/* The only code that uses a device address for T2. The first loads, the second stores, and both return. */
__attribute__((noinline)) uint32_t switch_t2_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

__attribute__((noinline)) void switch_t2_write32(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static uint32_t io_read(void *context, unsigned reg)
{
    (void)context;
    return switch_t2_access(reg, 0, 0);
}

static void io_write(void *context, unsigned reg, uint32_t value)
{
    (void)context;
    switch_t2_access(reg, 1, value);
}

static uint64_t io_now_us(void *context)
{
    (void)context;
    uint64_t count;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(count)::"memory");
    return count * 5u / 96u; /* 19.2 MHz, which the entry check has already required */
}

void switch_t2_run_on_hardware(struct switch_t2_image_result *result)
{
    static const struct switch_t2_io io = {0, SWITCH_T2_TIMING_0_PIN, SWITCH_T2_TIMING_1_PIN, io_read, io_write, io_now_us};
    switch_t2_run_with(&io, result);
}
