// SPDX-License-Identifier: MIT
// The range encode's copies of the images textures are reached through, and of texel buffers held in a pool of a
// block-linear surface's rows (raw_texel_rows.h) around the passes that render them and the blit commands that reach them
// (a draw's and a kernel's are refresh(), writtenBack() and the kernel shadows')
#import "RawMetalRangeEncode.h"
#include "raw_overlap_rows.h"
#include "raw_texel_rows.h"
#include "raw_xr_pools.h"

void rmMoveShadowImage(raw::BlitEncoder &b, const raw::Texture &t, bool fill)
{
    if (t.xrPlanes) {
        raw::moveXrPools(b, t, fill);
    } else if (t.texelRows) {
        raw::moveTexelRows(b, t, fill);
    } else if (fill) {
        b.copyTexels(t, 0, 0, 0, *t.sampled, 0, 0, 0, t.width, t.height);
    } else {
        raw::Texture scratch;
        b.copyTexels(*t.sampled, 0, 0, 0, raw::storedRows(t, t, scratch), 0, 0, 0, t.width, t.height);
    }
}

// A clear reaches the render area alone in a pass that draws (RawMetalRangeResolve.mm), and the pool goes back whole: it
// is filled before every pass into it
void RMRangeEncode::texelRowsTargets(const RMPass &pass)
{
    for (const RMPass::Color &a : pass.color) {
        __unsafe_unretained RMTexture *t = (RMTexture *)a.texture;
        if (!t || !t->_raw.texelRows)
            continue;
        raw::BlitEncoder *b = rc_->blit();
        raw::moveTexelRows(*b, t->_raw, true);
        b->end();
        if (a.store != MTLStoreActionDontCare &&
            std::find(shadowsWritten_.begin(), shadowsWritten_.end(), t) == shadowsWritten_.end())
            shadowsWritten_.push_back(t);
    }
}

void RMRangeEncode::blitTexelRows(const Cmd &c, bool before)
{
    __unsafe_unretained RMTexture *read = nil, *written = nil;
    switch (c.op) {
    case OpCopyTexture:
        read = (__bridge RMTexture *)c.obj;
        written = (__bridge RMTexture *)c.obj2;
        break;
    case OpFillTexture:
        written = (__bridge RMTexture *)c.obj;
        break;
    case OpCopyBufferToTexture:
        written = (__bridge RMTexture *)c.obj2;
        break;
    case OpCopyTextureToBuffer:
        read = (__bridge RMTexture *)c.obj;
        break;
    default:
        return;
    }
    if (read == written)
        read = nil;
    for (__unsafe_unretained RMTexture *t : {read, written}) {
        if (!t || !t->_raw.texelRows || (!before && t != written))
            continue;
        // the name encodeBlit orders the command by: every texture over the surface stands for its memory
        const void *surface = (t->_levelsOwner ?: t)->_surface;
        be_->access(surface, surface);
        raw::moveTexelRows(*be_, t->_raw, before);
    }
}
