#ifndef SWITCH_DIAG_T2_IMAGE_H
#define SWITCH_DIAG_T2_IMAGE_H
#include "t2_prelude.h"

/* What the T2 image reports: the controller words, the sensor sequence's result and the one stop code that ends the run. */
struct switch_t2_image_result {
    int stop;
    unsigned writes, transfers;
    struct switch_t2_probe probe;
    struct switch_t2_result sensor;
    int sensor_ran;
};

/* The whole run over any io: prelude, then the sensor sequence only when the prelude finished. */
void switch_t2_run_with(const struct switch_t2_io *io, struct switch_t2_image_result *result);

/* The loader's own io (t2_hw.c, with the raw accesses in t2_access.c). */
void switch_t2_run_on_hardware(struct switch_t2_image_result *result);
#endif
