/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_AWDL_RADIO_H
#define SWITCH_AWDL_RADIO_H

#include "awdl.h"

#define SWITCH_AWDL_CLOCK_UNCERTAINTY_NS 1000000u
/* Maximum assumed TSF drift between correlated samples. */
#define SWITCH_AWDL_CLOCK_DRIFT_PPM 500u
#define SWITCH_AWDL_CLOCK_JUMP_US   3000u

struct switch_awdl_radio_clock {
    struct awdl_tsf_clock affine;
    uint32_t epoch, uncertainty_ns, invalid_epoch;
    uint8_t renewal_attempted;
};

struct switch_awdl_radio_clock_request {
    uint8_t pending, blocked;
};

#ifdef __cplusplus
extern "C" {
#endif

/* 1: a new epoch or discontinuity; 0: same clock; -1: rejected sample. */
int switch_awdl_radio_clock_update(struct switch_awdl_radio_clock *clock, uint64_t tsf, uint64_t host_ns,
                                   uint32_t uncertainty_ns, uint32_t epoch, uint64_t now_ns);
int switch_awdl_radio_clock_valid(const struct switch_awdl_radio_clock *clock, uint64_t now_ns);
uint64_t switch_awdl_radio_clock_expires(const struct switch_awdl_radio_clock *clock);
uint64_t switch_awdl_radio_clock_deadline(const struct switch_awdl_radio_clock *clock);
int switch_awdl_radio_clock_try_renew(struct switch_awdl_radio_clock *clock, uint64_t now_ns);
void switch_awdl_radio_clock_clear(struct switch_awdl_radio_clock *clock);
uint64_t switch_awdl_radio_clock_tolerance_us(uint32_t previous_uncertainty_ns, uint32_t uncertainty_ns,
                                             uint64_t elapsed_ns);
int switch_awdl_radio_clock_request_begin(struct switch_awdl_radio_clock_request *request, int independent);
void switch_awdl_radio_clock_request_complete(struct switch_awdl_radio_clock_request *request, int error);

#ifdef __cplusplus
}
#endif
#endif
