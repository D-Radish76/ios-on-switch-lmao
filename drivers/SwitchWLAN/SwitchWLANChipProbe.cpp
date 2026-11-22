// SPDX-License-Identifier: BSD-2-Clause-Patent
// The stock allocator words read from the chip's RAM, and the bytes of our first announcements after the first
// activations as they leave for the chip.
#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <sys/errno.h>
#include "SwitchWLANProbeHooks.h"
#include "SwitchWLANA3Wire.h"
#include "awdl/awdl.h"

namespace {
// The firmware review's reading of 7.35.252.108: arena total, allocated, free-list head. Unverified, so logged raw.
constexpr uint32_t kHeapWords[] = {0x18102c, 0x18104c, 0x1df1ec};
constexpr unsigned kHeapCount = sizeof kHeapWords / sizeof kHeapWords[0];
constexpr size_t kEnvelopeHeader = 14 + sizeof(switch_a3_tx_header);
// The hex text and the other arguments share the kernel encoder's fixed 256-byte buffer; this keeps a line under half.
constexpr size_t kLineBytes = 32;
constexpr int kDumpsPerType = 2;
constexpr unsigned kDumpActivations = 2;

// Stored at every radio bring-up, read by the AWDL threads: the atomic keeps the store and loads whole.
const switch_wlan_probe_driver *gLent;

// Re-armed under the AWDL lock, spent by the A3 worker as frames leave.
int gPsfLeft, gMifLeft;
unsigned gDumpActivations;  // under the AWDL lock
unsigned gDumps;            // the A3 worker's

const switch_wlan_probe_driver *driver()
{
    return __atomic_load_n(&gLent, __ATOMIC_ACQUIRE);
}

bool announcement(const uint8_t *frame, size_t length, uint8_t *subtype)
{
    if (length < 31 || (frame[0] & 0xfc) != 0xd0 || frame[24] != 0x7f || memcmp(frame + 25, awdl_oui, 3) ||
        frame[28] != 0x08)
        return false;
    *subtype = frame[30];
    return *subtype == AWDL_SUBTYPE_PSF || *subtype == AWDL_SUBTYPE_MIF;
}

void dump(const char *type, unsigned number, const uint8_t *frame, size_t length)
{
    for (size_t offset = 0; offset < length; offset += kLineBytes) {
        const size_t count = length - offset < kLineBytes ? length - offset : kLineBytes;
        char hex[2 * kLineBytes + 1];
        for (size_t i = 0; i < count; ++i)
            snprintf(hex + 2 * i, 3, "%02x", frame[offset + i]);
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe tx %s dump %u length %u offset %u: %s", type, number,
               static_cast<unsigned>(length), static_cast<unsigned>(offset), hex);
    }
}
}

void switch_wlan_probe_attached(const switch_wlan_probe_driver *probeDriver)
{
    __atomic_store_n(&gLent, probeDriver, __ATOMIC_RELEASE);
    switch_wlan_probe_admission_prepare();
    switch_wlan_probe_heap("attached");
}

int switch_wlan_probe_ioctl(uint32_t cmd, const void *in, size_t inLength, void *out, size_t outLength)
{
    const switch_wlan_probe_driver *d = driver();
    return d ? d->ioctl(cmd, in, inLength, out, outLength) : -ENXIO;
}

bool switch_wlan_probe_reachable()
{
    const switch_wlan_probe_driver *d = driver();
    return d && d->reachable();
}

void switch_wlan_probe_heap(const char *moment)
{
    const switch_wlan_probe_driver *d = driver();
    uint32_t values[kHeapCount] = {};
    if (!d || !d->read(kHeapWords, values, kHeapCount)) {
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe heap %s: chip not reachable", moment);
        return;
    }
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe heap %s: 0x18102c=%#x 0x18104c=%#x 0x1df1ec=%#x", moment, values[0],
           values[1], values[2]);
}

void switch_wlan_probe_activated()
{
    if (gDumpActivations < kDumpActivations) {
        ++gDumpActivations;
        __atomic_store_n(&gPsfLeft, kDumpsPerType, __ATOMIC_RELAXED);
        __atomic_store_n(&gMifLeft, kDumpsPerType, __ATOMIC_RELAXED);
    }
    switch_wlan_probe_admission_activated();
}

void switch_wlan_probe_tx(const uint8_t *envelope, size_t length)
{
    uint8_t subtype = 0;
    if (length <= kEnvelopeHeader || !announcement(envelope + kEnvelopeHeader, length - kEnvelopeHeader, &subtype))
        return;
    int *left = subtype == AWDL_SUBTYPE_PSF ? &gPsfLeft : &gMifLeft;
    if (__atomic_load_n(left, __ATOMIC_RELAXED) <= 0)
        return;
    __atomic_sub_fetch(left, 1, __ATOMIC_RELAXED);
    dump(subtype == AWDL_SUBTYPE_PSF ? "PSF" : "MIF", ++gDumps, envelope + kEnvelopeHeader, length - kEnvelopeHeader);
}
#endif
