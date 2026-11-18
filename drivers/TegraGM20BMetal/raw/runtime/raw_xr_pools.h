// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 held in a surface's two planes (raw_xr_pass.h) whose plane rows or offset are 16 bytes but not 32
// apart: such a plane is sampled through its pool image (Texture::sampled), so the texture's header pair reads the
// pools. Every write lands in the rows (passes through their shadow backings, blits, CPU copies) except a kernel's,
// which stores through the pool header: the copy engine fills each pool from its rows before work reads the pair, and
// drains a pool a kernel wrote back into its rows as the kernel's encoder ends (raw_kernel_shadow.h).
// Planes whose rows overlap one another (a pitch below their row's bytes) are read as Metal reads them, each row at its
// pitch (an M4 Max over words of rows 64 and 16 for 32 texels reads every texel there), through pools the copy engine
// gathers. A pass renders into both pools, then each plane goes into its rows, each byte from the row whose store lands
// last in an M4 Max's pass (raw_overlap_order.h, which its 550 passes follow too), a kernel's in row order (an M4 Max's
// kernel stores race: five runs of one shape differ).
#pragma once
#include "raw_gpu.h"
#include "raw_overlap_rows.h"
#include "raw_xr_pass.h"

namespace raw {

inline bool xrPlanesPooled(const Texture &t)
{
    return t.xrPlanes && ((t.luma && t.luma->sampled) || (t.chroma && t.chroma->sampled));
}

// Held in planes whose rows overlap: stored through both pools
inline bool xrPlanesOverlap(const Texture &t)
{
    if (!t.xrPlanes || !t.luma || !t.chroma || !t.luma->sampled)
        return false;
    return overlapsRows(*t.luma) || (!t.chroma->memoryless && overlapsRows(*t.chroma));
}

// Planes that overlap from their pools after work rendered them there, each byte from the row whose store an M4 Max's
// pass lands last (pack overlap states), a plane whose rows do not overlap by the copy engine; 0, or 1 a target the raw
// path cannot render, 2 a draw the encoder refused (*why)
inline int storeXrPlaneRows(Device &d, CommandBuffer &rc, const Texture &t, const char **why)
{
    constexpr uint32_t kR8Unorm = 10;
    for (const Texture *plane : {t.chroma, t.luma}) {
        if (!plane->sampled)
            continue;
        if (!overlapsRows(*plane)) {
            BlitEncoder *b = rc.blit();
            b->copyTexels(*plane->sampled, 0, 0, 0, *plane, 0, 0, 0, plane->width, plane->height);
            b->end();
            continue;
        }
        const bool alpha = plane == t.chroma;
        const uint32_t format = alpha ? kR8Unorm : kXrWordFormat;
        Pipeline *state = d.ownPipeline(overlapState(format));
        if (!state) {
            *why = "the pack has no overlap state storing an RGB10A8_2P_XR10 plane whose rows overlap";
            return 1;
        }
        // the r8 state draws unorm bytes: the pair's alpha header reads the pool as R8Unorm
        Texture source = *plane->sampled;
        if (alpha) {
            source.format = kR8Unorm;
            source.tic = t.tic + 1;
        }
        const OverlapRows rows =
            overlapRowsOf(plane->pitch, plane->width * plane->bpp, plane->height, plane->bpp, plane->bpp, 1, 1);
        if (const int failed =
                storeOverlapRows(rc, state, format, plane->va, *plane, source, nullptr, rows, nullptr, why))
            return failed;
    }
    return 0;
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
