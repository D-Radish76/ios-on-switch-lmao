// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

// Every reason the raw path opens or uses its m2v device (RM_M2V). raw/plan/m2v_routes.tsv has a row for each, with
// what the site does once m2v is gone; raw/plan/test/m2v_routes_test.py keeps the two equal. The names are what debug
// builds log and what switchRawM2V's "needs" and "opened_for" hold.
enum RMNeed : uint8_t {
    // command buffers and indirect command buffers replayed on m2v
    NeedReplay,
    NeedReplayQueue,
    NeedRawStateTwinAtReplay,
    // what a library or function answers from its m2v twin
    NeedLibraryTwin,
    NeedFunctionTwin,
    // pipeline states made on m2v, or m2v twins of the raw path's
    NeedComputeStateTwin,
    NeedRenderStateTwin,
    NeedPackedRenderStateTwin,
    // twins of objects the raw path keeps itself
    NeedBufferTwin,
    NeedTextureTwin,
    NeedSamplerTwin,
    NeedDepthStencilTwin,
    NeedFenceTwin,
    // a device selector only m2v's device class has a method for (installM2VForwarders); logged with the selector
    NeedDeviceSelector,
    NeedHwTestDevice,
    NeedCount
};

static constexpr const char *kNeedNames[] = {
    "a command buffer replayed on m2v",
    "a command queue for a command buffer replayed on it",
    "a raw state's twin, at its first m2v replay",
    "a library's twin",
    "a function's twin",
    "a compute state's twin",
    "a render state's twin",
    "a packed render state's twin",
    "a buffer's twin",
    "a texture's twin",
    "a sampler's twin",
    "a depth/stencil state's twin",
    "a fence's twin",
    "m2v's -[MTLDevice selector]",
    "HW Test's m2v device",
};
static_assert(sizeof kNeedNames / sizeof *kNeedNames == NeedCount, "a name for each need");
