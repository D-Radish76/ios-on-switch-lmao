#include "t1v3.h"

#define CLK_L_I2C1   (1u << 12)
#define STATUS_BUSY  0x100u
#define STATUS_NOACK 0xfu
#define LOAD_PENDING 1u
#define UNREADABLE   0xffffffffu

struct run {
    const struct switch_t1v3_io *io;
    struct switch_t1v3_result *result;
};

static uint32_t get(struct run *run, unsigned reg)
{
    if (!switch_t1v3_may_read(reg)) {
        run->result->stop = SWITCH_T1V3_DENIED;
        return 0;
    }
    return run->io->read(run->io->context, reg);
}

static void put(struct run *run, unsigned reg, uint32_t value)
{
    if (!switch_t1v3_may_write(reg, value)) {
        run->result->stop = SWITCH_T1V3_DENIED;
        return;
    }
    run->io->write(run->io->context, reg, value);
    run->result->writes++;
}

static uint64_t now(struct run *run)
{
    return run->io->now_us(run->io->context);
}

/* Waits us microseconds. Ends by count too, so a stopped counter cannot hold the loader. */
static int wait_us(struct run *run, uint32_t us)
{
    const uint64_t start = now(run);
    for (unsigned polls = 0; now(run) - start < us; ++polls) {
        if (polls >= SWITCH_T1V3_POLL_LIMIT) {
            run->result->stop = SWITCH_T1V3_COUNTER_STALLED;
            return 0;
        }
    }
    return 1;
}

/* The counter every time limit rests on must advance before any device is touched. */
static int counter_advances(const struct switch_t1v3_io *io)
{
    const uint64_t first = io->now_us(io->context);
    for (unsigned i = 0; i < SWITCH_T1V3_POLL_LIMIT; ++i)
        if (io->now_us(io->context) != first)
            return 1;
    return 0;
}

/* Bit 0 clears by itself when the load is done. The register as a whole does not read 0 at rest on this silicon. */
static int load_configuration(struct run *run, uint32_t value)
{
    put(run, T1V3_I2C1_CONFIG_LOAD, value);
    if (run->result->stop)
        return 0;
    const uint64_t start = now(run);
    for (unsigned polls = 0; get(run, T1V3_I2C1_CONFIG_LOAD) & LOAD_PENDING; ++polls) {
        if (run->result->stop)
            return 0;
        if (polls >= SWITCH_T1V3_POLL_LIMIT || now(run) - start > SWITCH_T1V3_LOAD_TIMEOUT_US) {
            run->result->stop = SWITCH_T1V3_CONFIG_LOAD_TIMEOUT;
            return 0;
        }
    }
    return !run->result->stop;
}

static int read_car(struct run *run)
{
    struct switch_t1v3_result *r = run->result;
    r->rst_devices_l = get(run, T1V3_CAR_RST_DEVICES_L);
    r->clk_out_enb_l = get(run, T1V3_CAR_CLK_OUT_ENB_L);
    r->source_i2c1 = get(run, T1V3_CAR_CLK_SOURCE_I2C1);
    if (r->stop)
        return 0;
    if (!(r->clk_out_enb_l & CLK_L_I2C1))
        r->stop = SWITCH_T1V3_I2C_CLOCK_OFF;
    else if (r->rst_devices_l & CLK_L_I2C1)
        r->stop = SWITCH_T1V3_I2C_IN_RESET;
    else if (r->source_i2c1 != SWITCH_T1V3_SOURCE_EXPECTED)
        r->stop = SWITCH_T1V3_SOURCE_UNEXPECTED;
    return !r->stop;
}

static int timing_stop(uint32_t value, uint32_t expected)
{
    if (value == 0)
        return SWITCH_T1V3_TIMING_ZERO;
    if (value == UNREADABLE)
        return SWITCH_T1V3_TIMING_UNREADABLE;
    return expected && value != expected ? SWITCH_T1V3_TIMING_UNEXPECTED : 0;
}

/* Everything here is a read. TIMING_0 must be the T210 reset value Linux programs; TIMING_1 is only recorded, as long as
 * it is neither 0 nor all ones, because no source read gives its value on this silicon. */
static int read_controller(struct run *run)
{
    struct switch_t1v3_result *r = run->result;
    r->cnfg = get(run, T1V3_I2C1_CNFG);
    r->status = get(run, T1V3_I2C1_STATUS);
    r->int_status = get(run, T1V3_I2C1_INT_STATUS);
    r->divisor = get(run, T1V3_I2C1_CLK_DIVISOR);
    r->config_load = get(run, T1V3_I2C1_CONFIG_LOAD);
    r->timing_0 = get(run, T1V3_I2C1_TIMING_0);
    r->timing_1 = get(run, T1V3_I2C1_TIMING_1);
    if (r->stop)
        return 0;
    if (r->status & STATUS_BUSY)
        r->stop = SWITCH_T1V3_BUS_BUSY;
    else if (r->int_status)
        r->stop = SWITCH_T1V3_INT_PENDING;
    else if (r->divisor != SWITCH_T1V3_DIVISOR_BEFORE)
        r->stop = SWITCH_T1V3_DIVISOR_UNEXPECTED;
    else if (!(r->stop = timing_stop(r->timing_0, SWITCH_T1V3_TIMING_0)))
        r->stop = timing_stop(r->timing_1, 0);
    return !r->stop;
}

/* Three samples, spaced, all idle and all equal. */
static int bus_idle(struct run *run)
{
    struct switch_t1v3_result *r = run->result;
    for (unsigned i = 0; i < SWITCH_T1V3_IDLE_SAMPLES; ++i) {
        if (i && !wait_us(run, SWITCH_T1V3_IDLE_SPACING_US))
            return 0;
        const uint32_t status = get(run, T1V3_I2C1_STATUS), pending = get(run, T1V3_I2C1_INT_STATUS);
        if (r->stop)
            return 0;
        if ((status & STATUS_BUSY) || pending || status != r->status) {
            r->stop = SWITCH_T1V3_IDLE_FAILED;
            return 0;
        }
    }
    return 1;
}

/* The one change. Nothing is written back afterwards. */
static int apply_divisor(struct run *run)
{
    struct switch_t1v3_result *r = run->result;
    put(run, T1V3_I2C1_CLK_DIVISOR, SWITCH_T1V3_DIVISOR_AFTER);
    if (r->stop || !load_configuration(run, SWITCH_T1V3_LOAD_APPLY))
        return 0;
    r->load_after = get(run, T1V3_I2C1_CONFIG_LOAD);
    r->divisor_after = get(run, T1V3_I2C1_CLK_DIVISOR);
    r->status_load = get(run, T1V3_I2C1_STATUS);
    r->int_status_load = get(run, T1V3_I2C1_INT_STATUS);
    if (r->stop)
        return 0;
    if (r->divisor_after != SWITCH_T1V3_DIVISOR_AFTER)
        r->stop = SWITCH_T1V3_DIVISOR_READBACK;
    else if (r->status_load & STATUS_BUSY)
        r->stop = SWITCH_T1V3_NOT_IDLE;
    else if (r->int_status_load != r->int_status)
        r->stop = SWITCH_T1V3_INT_CHANGED;
    return !r->stop;
}

/* One normal mode transaction as hekate's _i2c_send_normal and _i2c_recv_normal do it, once, with no retry. The elapsed
 * time runs from go to the poll that saw BUSY clear and is kept as a reference value. */
static int transaction(struct run *run, int read, uint32_t pointer, struct switch_t1v3_transaction *timing)
{
    put(run, T1V3_I2C1_CMD_ADDR0, read ? 0x99u : 0x98u);
    if (!read)
        put(run, T1V3_I2C1_CMD_DATA1, pointer);
    put(run, T1V3_I2C1_CNFG, read ? 0x2840u : 0x2800u);
    if (run->result->stop || !load_configuration(run, SWITCH_T1V3_LOAD_TRANSFER))
        return 0;
    put(run, T1V3_I2C1_CNFG, read ? 0x2a40u : 0x2a00u);
    const uint64_t start = now(run);
    if (run->result->stop || !wait_us(run, SWITCH_T1V3_SETTLE_US))
        return 0;
    for (unsigned polls = 0; get(run, T1V3_I2C1_STATUS) & STATUS_BUSY; ++polls) {
        if (run->result->stop)
            return 0;
        timing->busy_seen = 1;
        if (polls >= SWITCH_T1V3_POLL_LIMIT || now(run) - start > SWITCH_T1V3_BUSY_TIMEOUT_US) {
            run->result->stop = SWITCH_T1V3_BUSY_TIMEOUT;
            return 0;
        }
    }
    timing->elapsed_us = (uint32_t)(now(run) - start);
    if (get(run, T1V3_I2C1_STATUS) & STATUS_NOACK) {
        run->result->stop = SWITCH_T1V3_NOACK;
        return 0;
    }
    run->result->transfers++;
    return !run->result->stop;
}

static void read_id(struct run *run)
{
    struct switch_t1v3_result *r = run->result;
    if (!transaction(run, 0, SWITCH_T1V3_ID_REGISTER, &r->transaction[0]) || !transaction(run, 1, 0, &r->transaction[1]))
        return;
    r->id = (uint8_t)(get(run, T1V3_I2C1_CMD_DATA1) & 0xffu);
    r->have_id = 1;
    if (r->id != SWITCH_T1V3_ID_VALUE)
        r->stop = SWITCH_T1V3_ID_MISMATCH;
}

void switch_t1v3_run(const struct switch_t1v3_io *io, struct switch_t1v3_result *result)
{
    struct run run = {io, result};
    /* A struct assignment would call memset, which this image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    if (!counter_advances(io)) {
        result->stop = SWITCH_T1V3_COUNTER_STALLED;
        return;
    }
    if (!read_car(&run) || !read_controller(&run) || !bus_idle(&run) || !apply_divisor(&run))
        return;
    read_id(&run);
    result->status_after = get(&run, T1V3_I2C1_STATUS);
    result->int_status_after = get(&run, T1V3_I2C1_INT_STATUS);
    if (!result->stop && (result->status_after & STATUS_BUSY))
        result->stop = SWITCH_T1V3_NOT_IDLE;
}
