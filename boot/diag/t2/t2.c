#include "t2.h"

static int get(const struct switch_t2_bus *bus, struct switch_t2_result *result, unsigned reg, uint8_t *value)
{
    if (bus->read(bus->context, reg, value)) {
        result->stop = T2_STOP_READ_FAILED;
        return 1;
    }
    return 0;
}

static int sample(const struct switch_t2_bus *bus, struct switch_t2_result *result, struct switch_t2_sample *out)
{
    return get(bus, result, T2_REMOTE_HIGH, &out->remote_high) || get(bus, result, T2_REMOTE_LOW, &out->remote_low) ||
           get(bus, result, T2_LOCAL_HIGH, &out->local_high) || get(bus, result, T2_LOCAL_LOW, &out->local_low);
}

/* The first status read at about 2 ms, none accepted as done before 31 ms, from 34 ms on every 5 ms, 100 ms at most. */
static int wait_for_conversion(const struct switch_t2_bus *bus, struct switch_t2_result *result, uint32_t start)
{
    uint32_t next = T2_FIRST_POLL_US;
    for (;;) {
        const uint32_t now = bus->now_us(bus->context) - start;
        if (now < next)
            bus->wait_us(bus->context, next - now);
        const uint32_t at = bus->now_us(bus->context) - start;
        if (get(bus, result, T2_STATUS, &result->status_last))
            return 1;
        if (result->status_last & T2_STATUS_BUSY) {
            if (!result->busy_seen)
                result->busy_first_us = at ? at : 1;
            result->busy_seen = 1;
        } else if (at >= T2_MIN_US) {
            result->done_us = at;
            return 0;
        }
        if (at >= T2_BOUND_US) {
            result->stop = T2_STOP_TIMEOUT;
            return 1;
        }
        next = at < T2_TYPICAL_US ? (at < T2_MIN_US ? T2_MIN_US : T2_TYPICAL_US) : at + T2_POLL_STEP_US;
    }
}

void switch_t2_run(const struct switch_t2_bus *bus, struct switch_t2_result *result)
{
    /* A struct assignment would call memset, which a loader image does not link. */
    for (volatile uint8_t *p = (volatile uint8_t *)result; p < (volatile uint8_t *)(result + 1); ++p)
        *p = 0;
    if (get(bus, result, T2_MANUFACTURER, &result->id))
        return;
    if (result->id != T2_MANUFACTURER_TI) {
        result->stop = T2_STOP_ID;
        return;
    }
    if (get(bus, result, T2_CONFIG, &result->config_before))
        return;
    if (result->config_before != T2_CONFIG_SHUTDOWN) {
        result->stop = T2_STOP_CONFIG;
        return;
    }
    if (get(bus, result, T2_OFFSET_HIGH, &result->offset_high) || get(bus, result, T2_OFFSET_LOW, &result->offset_low) ||
        get(bus, result, T2_STATUS, &result->status_before))
        return;
    if (result->status_before & T2_STATUS_BUSY) {
        result->stop = T2_STOP_BUSY_BEFORE;
        return;
    }
    if (sample(bus, result, &result->old_sample))
        return;

    const uint32_t start = bus->now_us(bus->context);
    ++result->writes;
    if (bus->write(bus->context, T2_ONE_SHOT, 0)) {
        result->stop = T2_STOP_WRITE_FAILED;
        return;
    }
    if (wait_for_conversion(bus, result, start))
        return;
    if (get(bus, result, T2_CONFIG, &result->config_after))
        return;
    result->not_in_shutdown = !(result->config_after & 0x40);
    uint8_t again = 0;
    if (get(bus, result, T2_REMOTE_HIGH, &result->new_sample.remote_high) ||
        get(bus, result, T2_REMOTE_LOW, &result->new_sample.remote_low) ||
        get(bus, result, T2_REMOTE_HIGH, &again) || get(bus, result, T2_LOCAL_HIGH, &result->new_sample.local_high) ||
        get(bus, result, T2_LOCAL_LOW, &result->new_sample.local_low))
        return;
    result->inconsistent = again != result->new_sample.remote_high;
}

int switch_t2_sixteenths(uint8_t high, uint8_t low)
{
    return (int)((unsigned)high << 4 | (low >> 4));
}

int switch_t2_at_range_edge(uint8_t high)
{
    return high == 0 || high == 127;
}
