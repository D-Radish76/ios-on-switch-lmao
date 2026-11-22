// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug-build measurements of the A3 radio (SwitchWLANChipProbe.cpp, SwitchWLANAdmissionProbe.cpp). Release builds
// get empty hooks.
#ifndef SWITCH_WLAN_PROBE_HOOKS_H
#define SWITCH_WLAN_PROBE_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#if DEVELOPMENT || DEBUG
// What SwitchWLAN.cpp lends the probes; thread context only, each call takes the driver's gate.
struct switch_wlan_probe_driver {
    // One ioctl on the station's interface: 0, SWLAN_FWERR(code) or a negative errno.
    int (*ioctl)(uint32_t cmd, const void *in, size_t inLength, void *out, size_t outLength);
    // The chip answers commands: the PCIe link is powered and no recovery is pending.
    bool (*reachable)();
    // Words of the chip's RAM, only while it is reachable and every address is inside it.
    bool (*read)(const uint32_t *addresses, uint32_t *values, unsigned count);
};

// Set while the admission sequence waits for a committed activation with the station up.
extern bool switch_wlan_probe_waiting;

void switch_wlan_probe_attached(const switch_wlan_probe_driver *driver);
void switch_wlan_probe_heap(const char *moment);
void switch_wlan_probe_tx(const uint8_t *envelope, size_t length);
// The station may receive frames addressed to other stations (the admission sequence's accept-all window).
bool switch_wlan_probe_accept_all();
bool switch_wlan_probe_awdl_committed();  // SwitchWLANAwdl.cpp

// Under the AWDL lock: each activation as it commits, and every radio refresh that finds it committed. Refreshes run
// per frame and per timer tick, so once nothing waits the hook is one flag test.
void switch_wlan_probe_activated();
void switch_wlan_probe_admission_try();

static inline void switch_wlan_probe_committed()
{
    if (__atomic_load_n(&switch_wlan_probe_waiting, __ATOMIC_RELAXED))
        switch_wlan_probe_admission_try();
}

int switch_wlan_probe_ioctl(uint32_t cmd, const void *in, size_t inLength, void *out, size_t outLength);
bool switch_wlan_probe_reachable();
void switch_wlan_probe_admission_prepare();
void switch_wlan_probe_admission_activated();
#else
static inline void switch_wlan_probe_heap(const char *)
{}

static inline void switch_wlan_probe_tx(const uint8_t *, size_t)
{}

static inline void switch_wlan_probe_activated()
{}

static inline void switch_wlan_probe_committed()
{}
#endif

#endif
