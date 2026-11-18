// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 held in a surface's two planes (raw_xr_pass.h) whose plane rows or offset are 16 bytes but not 32
// apart: such a plane is sampled through its pool image (Texture::sampled), so the texture's header pair reads the
// pools. Every write lands in the rows (passes through their shadow backings, blits, CPU copies) except a kernel's,
// which stores through the pool header: the copy engine fills each pool from its rows before work reads the pair, and
// drains a pool a kernel wrote back into its rows as the kernel's encoder ends (raw_kernel_shadow.h).
#pragma once
#include "raw_gpu.h"

namespace raw {

inline bool xrPlanesPooled(const Texture &t)
{
    return t.xrPlanes && ((t.luma && t.luma->sampled) || (t.chroma && t.chroma->sampled));
}

// `fill`: each pool from its rows; else each pool back into its rows
inline void moveXrPools(BlitEncoder &b, const Texture &t, bool fill)
{
    for (const Texture *plane : {t.luma, t.chroma})
        if (plane && plane->sampled) {
            const Texture &rows = *plane, &pool = *plane->sampled;
            b.copyTexels(fill ? rows : pool, 0, 0, 0, fill ? pool : rows, 0, 0, 0, plane->width, plane->height);
        }
}

inline void fillXrPools(CommandBuffer &rc, const Texture &t)
{
    if (!xrPlanesPooled(t))
        return;
    BlitEncoder *b = rc.blit();
    moveXrPools(*b, t, true);
    b->end();
}

}  // namespace raw
