#include <stdint.h>
#include "t3.h"

/* The only code that uses a device address for T3. One load and one store, each returning at once. */
__attribute__((noinline)) uint32_t switch_t3_read32(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

__attribute__((noinline)) void switch_t3_write32(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}

struct clocks {
    uint64_t start;
    uint64_t rail;
    int rail_marked;
    uint64_t window;
    unsigned window_ms;
};

static uint32_t io_read(void *context, unsigned reg)
{
    (void)context;
    return switch_t3_access(reg, 0, 0);
}

static void io_write(void *context, unsigned reg, uint32_t value)
{
    (void)context;
    switch_t3_access(reg, 1, value);
}

static int io_read_expired(void *context)
{
    return switch_t3_spent(((const struct clocks *)context)->start, SWITCH_T3_READ_BUDGET_MS);
}

static void io_rail_mark(void *context)
{
    struct clocks *clocks = context;
    clocks->rail = switch_t3_counter();
    clocks->rail_marked = 1;
}

static int io_rail_expired(void *context)
{
    const struct clocks *clocks = context;
    return !clocks->rail_marked || switch_t3_spent(clocks->rail, SWITCH_T3_RAIL_LIMIT_MS);
}

static void io_window_begin(void *context, unsigned milliseconds)
{
    struct clocks *clocks = context;
    clocks->window_ms = milliseconds;
    clocks->window = switch_t3_counter();
}

static int io_window_over(void *context)
{
    const struct clocks *clocks = context;
    return switch_t3_spent(clocks->window, clocks->window_ms);
}

void switch_t3_run_on_hardware(struct switch_t3_result *result, switch_t3_show_fn show)
{
    struct clocks clocks = {switch_t3_counter(), 0, 0, 0, 0};
    const struct switch_t3_io io = {
        &clocks, io_read, io_write, show, io_read_expired, io_rail_mark, io_rail_expired, io_window_begin, io_window_over,
    };
    switch_t3_run(&io, result);
}
