#ifndef SWITCH_DIAG_T2_PRELUDE_H
#define SWITCH_DIAG_T2_PRELUDE_H
#include "t2_bus.h"

/* The controller words the prelude read, for the screen. */
struct switch_t2_probe {
    uint32_t rst_devices_l, clk_out_enb_l, source_i2c1, pllp_base;
    uint32_t cnfg, status, int_status, divisor, config_load, timing_0, timing_1;
    uint32_t load_after, divisor_after, status_load, int_status_load;
};

/* Everything before the first sensor transfer: the counter, the CAR reads, the controller reads, the idle samples and the one
 * divisor write with its load. 1 when the sequence may go on; otherwise hw->stop says why and nothing more is written. */
int switch_t2_prelude(struct switch_t2_hw *hw, struct switch_t2_probe *probe);
#endif
