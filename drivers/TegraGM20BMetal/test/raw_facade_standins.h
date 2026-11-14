// SPDX-License-Identifier: MIT
// What the facade asks of the frontend and the bridge on a host with no m2v device (raw_facade_standins.mm): the frontend
// answers as one with no device would, and the argument layouts a test reads are the test's own values.
#pragma once
#import <Foundation/Foundation.h>
#include <cstdint>
#include <vector>
#include "../raw/runtime/raw_argument_layout.h"

namespace standin {

struct Layout {
    uint64_t encodedLength = 0, alignment = 1;
    std::vector<raw::ArgumentRun> runs;
};

// The layouts native_bridge_metal_argument_layouts answers for `twin`'s buffer `index`; none: it refuses
void functionLayouts(id twin, uint32_t index, std::vector<Layout> layouts);

}  // namespace standin
