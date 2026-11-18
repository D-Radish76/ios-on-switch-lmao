// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

// Every reason the raw path opens or uses its m2v device (RM_M2V). raw/plan/m2v_routes.tsv has a row for each, with
// what the site does once m2v is gone; raw/plan/test/m2v_routes_test.py keeps the two equal. The names are what debug
// builds log and what switchRawM2V's "needs" and "opened_for" hold.
enum RMNeed : uint8_t {
    // a device selector only m2v's device class has a method for (installM2VForwarders); logged with the selector
    NeedDeviceSelector,
    NeedHwTestDevice,
    NeedCount
};

static constexpr const char *kNeedNames[] = {
    "m2v's -[MTLDevice selector]",
    "HW Test's m2v device",
};
static_assert(sizeof kNeedNames / sizeof *kNeedNames == NeedCount, "a name for each need");
