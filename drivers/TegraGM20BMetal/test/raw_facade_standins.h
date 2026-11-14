// SPDX-License-Identifier: MIT
// What the facade asks of the frontend and the bridge on a host with no m2v device (raw_facade_standins.mm): the frontend
// answers as one with no device would, and the bridge's answers a test reads are the test's own values, counted.
#pragma once
#import <Foundation/Foundation.h>
#include <cstdint>

namespace standin {

constexpr uint32_t kTextureFlags = 0x25u << 16, kSamplerWord = 0x40;

struct Asks {
    uint32_t textureFlags = 0, samplerWords = 0;
};

Asks &asks();

}  // namespace standin
