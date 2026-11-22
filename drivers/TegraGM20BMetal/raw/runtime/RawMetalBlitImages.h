// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"

// Level `level` of the image a blit of `texture` moves: its levels owner's, or a view's own of another form
// (RawMetalDecodedFamily.mm)
inline bool blitImage(RMTexture *texture, uint32_t level, raw::Texture &image)
{
    RMTexture *owner = texture->_ownForm ? texture : texture->_levelsOwner ?: texture;
    const uint32_t absolute = texture->_ownForm ? level : texture->_levelsBase + level;
    if (absolute) {
        if (!raw::chainLevelImage(owner->_raw, absolute, image))
            return false;
    } else {
        image = owner->_raw;
    }
    image.va += uint64_t(texture->_ownForm ? 0 : texture->_slicesBase) * image.layerBytes;
    image.layers = texture->_raw.layers;
    image.type = texture->_raw.type;
    image.format = texture->_raw.format;
    image.stencilTemp = owner->_raw.stencilTemp;
    return true;
}

// A buffer<->texture copy of `planes` planes from plane z, `image` bytes apart in the buffer (RawMetalEncodeBlit.mm)
void rmCopyPlanes(raw::BlitEncoder *be, raw::CommandBuffer *rc, const raw::Buffer &b, uint64_t offset, uint64_t row,
                  uint64_t image, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
                  uint32_t planes, bool intoTexture, uint8_t aspect);
// The image a buffer copy of level `level` of `t` moves (RawMetalEncodeBlit.mm)
void rmBufferCopyImage(RMTexture *t, uint32_t level, raw::Texture &image, bool into);
