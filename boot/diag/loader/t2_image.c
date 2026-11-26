#include "t2_image.h"

static void clear(struct switch_t2_image_result *result)
{
    /* A struct assignment would call memset, which this image does not link. */
    for (volatile uint8_t *b = (volatile uint8_t *)result; b < (volatile uint8_t *)(result + 1); ++b)
        *b = 0;
}

void switch_t2_run_with(const struct switch_t2_io *io, struct switch_t2_image_result *result)
{
    clear(result);
    struct switch_t2_hw hw = {io, 0, 0, 0};
    if (switch_t2_prelude(&hw, &result->probe)) {
        struct switch_t2_bus bus;
        switch_t2_hw_bus(&hw, &bus);
        switch_t2_run(&bus, &result->sensor);
        result->sensor_ran = 1;
    }
    result->stop = hw.stop ? hw.stop : result->sensor.stop;
    result->writes = hw.writes;
    result->transfers = hw.transfers;
}
