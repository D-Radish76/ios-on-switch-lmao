// SPDX-License-Identifier: MIT
// A sampler under a converted texture's address map (Texture::addressMap), and the map a kernel's sampler entry whose pairs
// its record holds (raw_pack.h kernelSamplerPairs) binds its header under: a draw's handle pairs each texture with its
// sampler, a kernel's sampler is a word of its own.
#pragma once
#include <cstdint>
#include <cstring>

namespace raw {

// An MTLSamplerAddressMode under address map `map` (m2v texture/ycbcr_image.rs `ycbcr_modes`, Metal's measured reads): bit
// 0 sends the wrapping modes (mirror clamp to edge, repeat, mirrored repeat) to clamp to edge, bit 1 the clamps to zero
// and to the border colour
constexpr uint8_t mappedAddressMode(uint8_t mode, uint8_t map)
{
    return ((map & 1) && mode >= 1 && mode <= 3) || ((map & 2) && (mode == 4 || mode == 5)) ? 0 : mode;
}

// The same on a header's TEXSAMP0 ADDRESS_U, _V and _P values, as samplerHeader and NVK write the modes (WRAP 0, MIRROR 1,
// CLAMP_TO_EDGE 2, BORDER 3, MIRROR_ONCE_CLAMP_TO_EDGE 5)
constexpr uint32_t mappedHeaderAddress(uint32_t field, uint8_t map)
{
    return ((map & 1) && (field == 0 || field == 1 || field == 5)) || ((map & 2) && field == 3) ? 2 : field;
}

inline void mapSamplerHeader(const uint32_t in[8], uint8_t map, uint32_t out[8])
{
    memcpy(out, in, 8 * sizeof(uint32_t));
    for (const uint32_t shift : {0u, 3u, 6u})
        out[0] = (out[0] & ~(7u << shift)) | mappedHeaderAddress(in[0] >> shift & 7, map) << shift;
}

// A bound sampler's modes under `map`: one value for each header they give
constexpr uint32_t mappedModes(uint8_t s, uint8_t t, uint8_t r, uint8_t map)
{
    return mappedAddressMode(s, map) | uint32_t(mappedAddressMode(t, map)) << 8 |
           uint32_t(mappedAddressMode(r, map)) << 16;
}

// The map of the texture at the entry's first paired slot (`mapOf`, 0 where none is bound), where every paired texture's
// map gives the sampler the same header (`header(m)`, one value for each header under map m). False where two give it
// different headers, which one word cannot hold.
template <typename Header, typename MapOf>
bool pairedSamplerMap(uint32_t pairs, Header header, MapOf mapOf, uint8_t &map)
{
    map = 0;
    for (uint32_t s = pairs, first = 1; s; s &= s - 1, first = 0) {
        const uint8_t m = mapOf(uint32_t(__builtin_ctz(s)));
        if (first)
            map = m;
        else if (m != map && header(m) != header(map))
            return false;
    }
    return true;
}

}  // namespace raw
