// SPDX-License-Identifier: BSD-2-Clause-Patent
#if DEVELOPMENT || DEBUG
#include <kern/thread_call.h>
#include <os/log.h>
#include "SwitchWLANProbeHooks.h"

namespace {
// Stock firmware 7.35.252.108's allocator: arena total, bytes in use, block-header overhead in use (8 bytes a block),
// high-water mark of in-use plus overhead, failed allocations; then the failures of packet-buffer allocations, of
// packet-ID allocations and of the shared-pool fallback.
constexpr uint32_t kHeapWords[] = {0x18102c, 0x18104c, 0x181074, 0x1df200, 0x181028, 0x1df2e8, 0x1df220, 0x1df418};
constexpr unsigned kHeapCount = sizeof kHeapWords / sizeof kHeapWords[0];

// Stored at every radio bring-up, read by the worker and the AWDL start thread call.
switch_wlan_probe_read gRead;
thread_call_t gAwdlStart;

void awdlStart(thread_call_param_t, thread_call_param_t)
{
    switch_wlan_probe_heap("awdl-start");
}

bool enter(thread_call_t &call)
{
    const thread_call_t c = __atomic_load_n(&call, __ATOMIC_ACQUIRE);
    if (c)
        thread_call_enter(c);
    return c != nullptr;
}
}

void switch_wlan_probe_attached(switch_wlan_probe_read read)
{
    __atomic_store_n(&gRead, read, __ATOMIC_RELEASE);
    if (!gAwdlStart)
        __atomic_store_n(&gAwdlStart, thread_call_allocate(awdlStart, nullptr), __ATOMIC_RELEASE);
    switch_wlan_probe_heap("attached");
}

void switch_wlan_probe_heap(const char *moment)
{
    const switch_wlan_probe_read read = __atomic_load_n(&gRead, __ATOMIC_ACQUIRE);
    uint32_t v[kHeapCount] = {};
    if (!read || !read(kHeapWords, v, kHeapCount)) {
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe heap %s: chip not reachable", moment);
        return;
    }
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe heap %s: total %u in-use %u overhead %u free %u high-water %u failed %u "
           "packet-failed %u packet-id-failed %u pool-failed %u",
           moment, v[0], v[1], v[2], v[0] - v[1] - v[2], v[3], v[4], v[5], v[6], v[7]);
}

void switch_wlan_probe_activated()
{
    static bool started;
    if (!started)
        started = enter(gAwdlStart);
}
#endif
