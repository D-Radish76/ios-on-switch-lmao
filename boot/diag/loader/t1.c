#include "t1.h"

#define CLK_L_I2C1 (1u << 12)
#define CLK_L_PWM  (1u << 17)
#define STATUS_BUSY 0x100u
#define STATUS_NOACK 0xfu
#define LOAD_PENDING 1u

struct run {
    const struct switch_t1_io *io;
    struct switch_t1_result *result;
};

static uint32_t get(struct run *run, unsigned reg)
{
    if (!switch_t1_may_read(reg)) {
        run->result->stop = SWITCH_T1_DENIED;
        return 0;
    }
    return run->io->read(run->io->context, reg);
}

static void put(struct run *run, unsigned reg, uint32_t value)
{
    if (!switch_t1_may_write(reg, value)) {
        run->result->stop = SWITCH_T1_DENIED;
        return;
    }
    run->io->write(run->io->context, reg, value);
}

int switch_t1_i2c1_clocked(const struct switch_t1_result *result)
{
    return (result->clk_out_enb_l & CLK_L_I2C1) != 0;
}

int switch_t1_pwm_clocked(const struct switch_t1_result *result)
{
    return (result->clk_out_enb_l & CLK_L_PWM) != 0;
}

static void read_car(struct run *run)
{
    struct switch_t1_result *r = run->result;
    r->rst_devices_l = get(run, T1_CAR_RST_DEVICES_L);
    r->clk_out_enb_l = get(run, T1_CAR_CLK_OUT_ENB_L);
    r->source_pwm = get(run, T1_CAR_CLK_SOURCE_PWM);
    r->source_i2c1 = get(run, T1_CAR_CLK_SOURCE_I2C1);
}

static void read_controller(struct run *run)
{
    struct switch_t1_result *r = run->result;
    r->pad_scl = get(run, T1_PINMUX_I2C1_SCL);
    r->pad_sda = get(run, T1_PINMUX_I2C1_SDA);
    r->cnfg = get(run, T1_I2C1_CNFG);
    r->status = get(run, T1_I2C1_STATUS);
    r->int_status = get(run, T1_I2C1_INT_STATUS);
    r->divisor = get(run, T1_I2C1_CLK_DIVISOR);
    r->config_load = get(run, T1_I2C1_CONFIG_LOAD);
}

/* hekate's _i2c_load_cfg_wait, with a bound in time instead of 20 polls, and a stop instead of going on. */
static int load_configuration(struct run *run)
{
    put(run, T1_I2C1_CONFIG_LOAD, 0x25u);
    const uint64_t start = run->io->now_us(run->io->context);
    for (unsigned polls = 0; get(run, T1_I2C1_CONFIG_LOAD) & LOAD_PENDING; ++polls) {
        if (run->result->stop)
            return 0;
        if (polls >= SWITCH_T1_POLL_LIMIT || run->io->now_us(run->io->context) - start > SWITCH_T1_LOAD_TIMEOUT_US) {
            run->result->stop = SWITCH_T1_CONFIG_LOAD_TIMEOUT;
            return 0;
        }
    }
    return !run->result->stop;
}

/* Wait SWITCH_T1_SETTLE_US after go before the first BUSY poll. Ends by count too, so a stopped counter cannot hold it. */
static int settle(struct run *run, uint64_t start)
{
    for (unsigned polls = 0; run->io->now_us(run->io->context) - start < SWITCH_T1_SETTLE_US; ++polls) {
        if (polls >= SWITCH_T1_POLL_LIMIT) {
            run->result->stop = SWITCH_T1_COUNTER_STALLED;
            return 0;
        }
    }
    return 1;
}

/* The counter T1's time limits rest on must advance before any device is touched. */
static int counter_advances(const struct switch_t1_io *io)
{
    const uint64_t first = io->now_us(io->context);
    for (unsigned i = 0; i < SWITCH_T1_POLL_LIMIT; ++i)
        if (io->now_us(io->context) != first)
            return 1;
    return 0;
}

/* One normal mode transaction as hekate's _i2c_send_normal and _i2c_recv_normal do it: address, data, configuration,
 * load, go, wait for BUSY to clear, then the NOACK bits. Once, with no retry. */
static int transaction(struct run *run, int read, uint32_t pointer)
{
    put(run, T1_I2C1_CMD_ADDR0, read ? 0x99u : 0x98u);
    if (!read)
        put(run, T1_I2C1_CMD_DATA1, pointer);
    put(run, T1_I2C1_CNFG, read ? 0x2840u : 0x2800u);
    if (run->result->stop || !load_configuration(run))
        return 0;
    put(run, T1_I2C1_CNFG, read ? 0x2a40u : 0x2a00u);
    const uint64_t start = run->io->now_us(run->io->context);
    if (!settle(run, start))
        return 0;
    for (unsigned polls = 0; get(run, T1_I2C1_STATUS) & STATUS_BUSY; ++polls) {
        if (run->result->stop)
            return 0;
        if (polls >= SWITCH_T1_POLL_LIMIT || run->io->now_us(run->io->context) - start > SWITCH_T1_BUSY_TIMEOUT_US) {
            run->result->stop = SWITCH_T1_BUSY_TIMEOUT;
            return 0;
        }
    }
    if (get(run, T1_I2C1_STATUS) & STATUS_NOACK) {
        run->result->stop = SWITCH_T1_NOACK;
        return 0;
    }
    run->result->transfers++;
    return !run->result->stop;
}

static void read_registers(struct run *run)
{
    struct switch_t1_result *r = run->result;
    for (unsigned i = 0; i < SWITCH_T1_REGISTERS; ++i) {
        if (!transaction(run, 0, switch_t1_tmp451_register[i]) || !transaction(run, 1, 0))
            return;
        r->value[i] = (uint8_t)(get(run, T1_I2C1_CMD_DATA1) & 0xffu);
        r->have |= 1u << i;
        if (i == 0 && r->value[0] != 0x55) {
            r->stop = SWITCH_T1_ID_MISMATCH;
            return;
        }
    }
}

void switch_t1_run(const struct switch_t1_io *io, struct switch_t1_result *result)
{
    struct run run = {io, result};
    /* A struct assignment would call memset, which this image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    if (!counter_advances(io)) {
        result->stop = SWITCH_T1_COUNTER_STALLED;
        return;
    }
    read_car(&run);
    if (result->stop)
        return;
    if (!switch_t1_i2c1_clocked(result)) {
        result->stop = SWITCH_T1_I2C_CLOCK_OFF;
        return;
    }
    if (result->rst_devices_l & CLK_L_I2C1) {
        result->stop = SWITCH_T1_I2C_IN_RESET;
        return;
    }
    read_controller(&run);
    if (result->stop)
        return;
    if (result->status & STATUS_BUSY)
        result->stop = SWITCH_T1_BUS_BUSY;
    else if (result->divisor != SWITCH_T1_DIVISOR_HEKATE)
        result->stop = SWITCH_T1_DIVISOR_UNEXPECTED;
    if (result->stop)
        return;
    read_registers(&run);
    result->status_after = get(&run, T1_I2C1_STATUS);
    result->int_status_after = get(&run, T1_I2C1_INT_STATUS);
    if (!result->stop && (result->status_after & STATUS_BUSY))
        result->stop = SWITCH_T1_NOT_IDLE;
}
