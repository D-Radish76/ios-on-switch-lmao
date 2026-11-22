// SPDX-License-Identifier: BSD-2-Clause-Patent
// Whether the receiver discards group-addressed AWDL frames before the payload's hook while the station is
// associated. From a committed activation with the station up: three windows of the payload's receive counters,
// baseline, WLC_SET_PROMISC 1, WLC_SET_PROMISC 0. The hook count is the control: promiscuous reception must raise it,
// or a zero in the announcement counters says nothing. A sequence that ends incomplete waits for the next one, up to
// kAttempts a boot.
#if DEVELOPMENT || DEBUG
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include "SwitchWLANA3Diagnostic.h"
#include "SwitchWLANA3Radio.h"
#include "SwitchWLANA3Wire.h"
#include "SwitchWLANProbeHooks.h"
#include "SwitchWLANStation.h"
#include "awdl/awdl.h"
#include "awdl/awdl_bcm.h"

namespace {
constexpr uint32_t kGetPromisc = 9, kSetPromisc = 10;
constexpr uint32_t kWindowSeconds = 20;
// Clear of the A3 worker's request numbers (from 1) and the TX probe's (0x7f000001).
constexpr uint32_t kRequestBase = 0x7e000000;
constexpr unsigned kWindows = 3;
constexpr unsigned kAttempts = 3;

struct Reading {
    bool valid, committed;
    uint64_t ns;
    uint8_t home, channel;  // the chip's home channel; the station's access point
    uint32_t station, activations;
    struct switch_a3_diagnostic diagnostic;
};

struct Window {
    char name;
    const char *what;
    int promisc;   // set as the window starts; -1 leaves it alone
    int expected;  // what WLC_GET_PROMISC must read back
};

struct Result {
    bool valid;
    uint32_t delta[SWITCH_A3_DIAG_COUNTERS];
};

constexpr Window kWindow[kWindows] = {
    {'a', "baseline", -1, 0}, {'b', "promiscuous", 1, 1}, {'c', "promiscuous off", 0, 0}};

thread_call_t gStep;
uint32_t gActivations;  // incremented under the AWDL lock, read by gStep's callback
bool gAcceptAll;        // read by the work loop for every EAPOL frame
// The rest belongs to gStep's callback, which runs once at a time and re-arms itself.
unsigned gNext, gAttempt;
uint32_t gRequest;
int gSetError;
Reading gOpened;
Result gResult[kWindows];

uint64_t hostNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

Reading readCounters()
{
    Reading reading = {};
    swlan_awdl_state state = {};
    switch_a3_control request = {};
    request.magic = SWITCH_A3_CONTROL_MAGIC;
    request.version = SWITCH_A3_WIRE_VERSION;
    request.header_bytes = sizeof request;
    request.total_bytes = sizeof request;
    request.request_id = kRequestBase + ++gRequest;
    request.session = switch_wlan_a3_radio_state(&state) ? 0 : state.generation;
    request.operation = SWITCH_A3_GET_STATE;
    uint8_t response[sizeof(switch_a3_reply) + sizeof(struct switch_a3_diagnostic)] = {};
    switch_a3_reply reply = {};
    size_t returned = 0;
    const int error = switch_wlan_a3_control(&request, sizeof request, response, sizeof response, &returned);
    reading.valid =
        !error && returned == sizeof response && switch_a3_reply_decode(response, sizeof reply, &request, &reply) &&
        switch_a3_diagnostic_decode(response + sizeof reply, sizeof reading.diagnostic, reply, reading.diagnostic);
    reading.ns = hostNs();
    reading.committed = switch_wlan_probe_awdl_committed();
    reading.activations = __atomic_load_n(&gActivations, __ATOMIC_RELAXED);
    reading.home = bcm_chanspec_primary(reply.home_chanspec);
    swlan_status status = {};
    switch_wlan_status_get(&status);
    reading.station = status.state;
    reading.channel = awdl_chan_of_freq(static_cast<uint16_t>(status.freq_mhz)).number;
    if (!reading.valid)
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: counter read failed: error %d, %u bytes", error,
               static_cast<unsigned>(returned));
    return reading;
}

int setPromisc(int value)
{
    const uint8_t le[4] = {static_cast<uint8_t>(value), 0, 0, 0};
    return switch_wlan_probe_ioctl(kSetPromisc, le, sizeof le, nullptr, 0);
}

int getPromisc(uint32_t &value)
{
    uint8_t le[4] = {};
    const int error = switch_wlan_probe_ioctl(kGetPromisc, le, sizeof le, le, sizeof le);
    value = le[0] | static_cast<uint32_t>(le[1]) << 8 | static_cast<uint32_t>(le[2]) << 16 |
            static_cast<uint32_t>(le[3]) << 24;
    return error;
}

void logAccessPoint()
{
    swlan_status status = {};
    switch_wlan_status_get(&status);
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe ap: channel %u, %u MHz, width %u MHz, centre %u, station state %u",
           awdl_chan_of_freq(static_cast<uint16_t>(status.freq_mhz)).number, status.freq_mhz, status.bw_mhz,
           status.center, status.state);
}

bool reachable(const Window &window, const char *boundary)
{
    if (switch_wlan_probe_reachable())
        return true;
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: window %c %s: chip not reachable", window.name, boundary);
    return false;
}

bool begin(const Window &window)
{
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: starting window %c (%s)%s", window.name, window.what,
           window.promisc < 0 ? ""
           : window.promisc   ? ", WLC_SET_PROMISC 1"
                              : ", WLC_SET_PROMISC 0");
    if (!reachable(window, "start"))
        return false;
    gSetError = 0;
    if (window.promisc >= 0) {
        // On before the chip can pass foreign frames up; off once it is known not to.
        if (window.promisc)
            __atomic_store_n(&gAcceptAll, true, __ATOMIC_RELEASE);
        gSetError = setPromisc(window.promisc);
        if (gSetError)
            os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: window %c: WLC_SET_PROMISC %d failed: %d", window.name,
                   window.promisc, gSetError);
        if (window.promisc ? gSetError != 0 : gSetError == 0)
            __atomic_store_n(&gAcceptAll, false, __ATOMIC_RELEASE);
    }
    gOpened = readCounters();
    return true;
}

bool finish(const Window &window, Result &result)
{
    if (!reachable(window, "end"))
        return false;
    const Reading closed = readCounters();
    uint32_t promisc = 0;
    const int promiscError = getPromisc(promisc);
    // A chip reload zeroes the counters and starts a new session; regrown counters alone would pass.
    bool counted = gOpened.valid && closed.valid && gOpened.diagnostic.session == closed.diagnostic.session;
    for (unsigned i = 0; counted && i < SWITCH_A3_DIAG_COUNTERS; ++i) {
        counted = closed.diagnostic.counters[i] >= gOpened.diagnostic.counters[i];
        result.delta[i] = closed.diagnostic.counters[i] - gOpened.diagnostic.counters[i];
    }
    // A Mac only announces to us while AWDL runs: a window that lost the activation, even briefly, proves nothing.
    const bool committed = gOpened.committed && closed.committed && gOpened.activations == closed.activations;
    const bool setting = !gSetError && (promiscError || promisc == static_cast<uint32_t>(window.expected));
    result.valid = counted && committed && setting && gOpened.station == closed.station &&
                   gOpened.channel == closed.channel && gOpened.home == closed.home;
    if (!result.valid) {
        os_log(OS_LOG_DEFAULT,
               "SwitchWLAN: probe admission window %c invalid: readings %u/%u, session %u->%u, station %u->%u, "
               "channel %u->%u, home %u->%u",
               window.name, gOpened.valid, closed.valid, gOpened.diagnostic.session, closed.diagnostic.session,
               gOpened.station, closed.station, gOpened.channel, closed.channel, gOpened.home, closed.home);
        os_log(OS_LOG_DEFAULT,
               "SwitchWLAN: probe admission window %c invalid: committed %u->%u, activations %u->%u, "
               "WLC_SET_PROMISC status %d, WLC_GET_PROMISC %u status %d",
               window.name, gOpened.committed, closed.committed, gOpened.activations, closed.activations, gSetError,
               promisc, promiscError);
    }
    if (!counted)
        return false;
    // Split so each line's arguments stay well inside the kernel encoder's fixed 256-byte buffer (log_encode.c).
    const uint32_t *d = result.delta;
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe admission window %c rx: %u ms, home %u->%u, WLC_GET_PROMISC %u "
           "status %d; hook %u header %u phy %u action %u prefix-PSF %u prefix-MIF %u bssid %u destination %u "
           "timestamp %u split %u export %u drop %u fetch-failed %u",
           window.name, static_cast<unsigned>((closed.ns - gOpened.ns) / 1000000), gOpened.home, closed.home, promisc,
           promiscError, d[SWITCH_A3_DIAG_RX_HOOK], d[SWITCH_A3_DIAG_RX_HEADER], d[SWITCH_A3_DIAG_RX_PHY],
           d[SWITCH_A3_DIAG_RX_ACTION], d[SWITCH_A3_DIAG_RX_PSF], d[SWITCH_A3_DIAG_RX_MIF], d[SWITCH_A3_DIAG_RX_BSSID],
           d[SWITCH_A3_DIAG_RX_DEST], d[SWITCH_A3_DIAG_RX_MATCH], d[SWITCH_A3_DIAG_RX_SPLIT],
           d[SWITCH_A3_DIAG_RX_EXPORT], d[SWITCH_A3_DIAG_RX_DROP], d[SWITCH_A3_DIAG_RX_FETCH_FAILED]);
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe admission window %c radio: excursions started %u entered %u returned %u; "
           "gate allow %u deny %u",
           window.name, d[SWITCH_A3_DIAG_EXCURSION_START], d[SWITCH_A3_DIAG_EXCURSION_ENTER],
           d[SWITCH_A3_DIAG_EXCURSION_RETURN], d[SWITCH_A3_DIAG_GATE_ALLOW], d[SWITCH_A3_DIAG_GATE_DENY]);
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe admission window %c tx: ingress %u native %u status %u status-drop %u; "
           "schedule block %u fault %u",
           window.name, d[SWITCH_A3_DIAG_TX_INGRESS], d[SWITCH_A3_DIAG_TX_NATIVE], d[SWITCH_A3_DIAG_TX_STATUS],
           d[SWITCH_A3_DIAG_TX_STATUS_EXPORT_DROP], d[SWITCH_A3_DIAG_SCHED_BLOCK], d[SWITCH_A3_DIAG_SCHED_FAULT]);
    return result.valid;
}

const char *verdict(bool controlRose, uint32_t baseline, uint32_t promiscuous, uint32_t after)
{
    if (baseline)
        return "announcements admitted without promiscuous";
    if (promiscuous && !after)
        return "receiver discards announcements unless promiscuous";
    if (promiscuous)
        return "inconclusive: announcements heard after promiscuous too";
    if (controlRose)
        return "no announcements even promiscuous";
    return "inconclusive: hook did not rise";
}

bool summarise()
{
    const Result &a = gResult[0], &b = gResult[1], &c = gResult[2];
    if (!a.valid || !b.valid || !c.valid) {
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission summary: incomplete, windows valid a %u b %u c %u", a.valid,
               b.valid, c.valid);
        return false;
    }
    const auto hook = [](const Result &r) {
        return r.delta[SWITCH_A3_DIAG_RX_HOOK];
    };
    const auto heard = [](const Result &r) {
        return r.delta[SWITCH_A3_DIAG_RX_PSF] + r.delta[SWITCH_A3_DIAG_RX_MIF];
    };
    const bool rose = 2ull * hook(b) >= 3ull * hook(a) && 2ull * hook(b) >= 3ull * hook(c) && hook(b) >= hook(a) + 100;
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe admission summary: control %s (hook a %u b %u c %u; rule b >= 1.5x a "
           "and c, b >= a + 100); %s",
           rose ? "rose" : "flat", hook(a), hook(b), hook(c), verdict(rose, heard(a), heard(b), heard(c)));
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe admission summary announcements: prefix-PSF a %u b %u c %u; "
           "prefix-MIF a %u b %u c %u; bssid a %u b %u c %u",
           a.delta[SWITCH_A3_DIAG_RX_PSF], b.delta[SWITCH_A3_DIAG_RX_PSF], c.delta[SWITCH_A3_DIAG_RX_PSF],
           a.delta[SWITCH_A3_DIAG_RX_MIF], b.delta[SWITCH_A3_DIAG_RX_MIF], c.delta[SWITCH_A3_DIAG_RX_MIF],
           a.delta[SWITCH_A3_DIAG_RX_BSSID], b.delta[SWITCH_A3_DIAG_RX_BSSID], c.delta[SWITCH_A3_DIAG_RX_BSSID]);
    return true;
}

void restoreAcceptAll()
{
    if (!__atomic_load_n(&gAcceptAll, __ATOMIC_ACQUIRE))
        return;
    if (!switch_wlan_probe_reachable()) {
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: accept-all left on, chip not reachable");
        return;
    }
    const int error = setPromisc(0);
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: accept-all restored off, WLC_SET_PROMISC 0 status %d", error);
    if (!error)
        __atomic_store_n(&gAcceptAll, false, __ATOMIC_RELEASE);
}

void conclude()
{
    restoreAcceptAll();
    switch_wlan_probe_heap("admission-end");
    const bool complete = summarise();
    gNext = 0;
    if (!complete && gAttempt < kAttempts)
        __atomic_store_n(&switch_wlan_probe_waiting, true, __ATOMIC_RELEASE);
}

void step(thread_call_param_t, thread_call_param_t)
{
    if (!gNext) {
        ++gAttempt;
        memset(gResult, 0, sizeof gResult);
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: attempt %u of %u", gAttempt, kAttempts);
        logAccessPoint();
        switch_wlan_probe_heap("admission-start");
    } else if (!finish(kWindow[gNext - 1], gResult[gNext - 1])) {
        conclude();
        return;
    }
    if (gNext == kWindows || !begin(kWindow[gNext++])) {
        conclude();
        return;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(kWindowSeconds, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(gStep, deadline);
}
}

bool switch_wlan_probe_waiting;

// Every bring-up loads the firmware afresh, with accept-all off.
void switch_wlan_probe_admission_prepare()
{
    __atomic_store_n(&gAcceptAll, false, __ATOMIC_RELEASE);
    if (gStep)
        return;
    gStep = thread_call_allocate_with_options(step, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (gStep)
        __atomic_store_n(&switch_wlan_probe_waiting, true, __ATOMIC_RELEASE);
    else
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe admission: no timer, sequence off");
}

void switch_wlan_probe_admission_try()
{
    if (swlan_awdl_infra_freq() && __atomic_exchange_n(&switch_wlan_probe_waiting, false, __ATOMIC_ACQ_REL))
        thread_call_enter(gStep);
}

void switch_wlan_probe_admission_activated()
{
    __atomic_add_fetch(&gActivations, 1, __ATOMIC_RELAXED);
}

bool switch_wlan_probe_accept_all()
{
    return __atomic_load_n(&gAcceptAll, __ATOMIC_ACQUIRE);
}
#endif
