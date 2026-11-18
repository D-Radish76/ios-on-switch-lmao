// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 and RGB10A8_2P_XR10_sRGB textures over a two-plane surface on the raw path, drawn and sampled in their
// planes (raw_xr_pass.h, raw_xr_texture.cpp; the sRGB one drawn through its format stand-in, raw_format_standin.h)
#pragma once
#import "RawMetalInternal.h"

// 550 or 551 over plane 0 of a two-plane surface whose rows hold its words and its alpha bytes and whose planes are no shorter
// than it: a 2D texture, one level, one sample, no rotation, of any usage. Metal reads the planes so whatever their elements
// (an M4 Max over 4+1, 4+2, 8+1, 2+1, 4+4 and '420v' planes: m2v-textures5 probes/xrplane0.mm), their top-left corner
bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
// False with raw::surfaceRefusal naming the step
bool makeXrPlanes(RMTexture *t, IOSurfaceRef s);
// 550 or 551 over one plane alone (a surface of one, or a plane of several): an M4 Max reads BGR10_XR's (BGR10_XR_sRGB's)
// words there, alpha 0 (m2v-textures3 probes/xrsurf.mm, m2v-textures4 probes/reads.mm), which the raw path holds under that
// format's header, its stand-in reading alpha 0. The format those words are held as, and whether `t` is held so
uint32_t xrWordsFormat(uint32_t format);
bool xrWordsTexture(RMTexture *t);
void readAlphaZero(raw::Texture &standIn);
// replaceRegion/getBytes of a texture held in its planes: plane 0's colour words alone, as Metal's CPU copies and blits move
// them (raw_xr.h), the alpha plane as it is. False: not such a texture, or a region off its one level and slice
bool xrPlaneWords(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
                  bool write);
// A pass that stores into `t` (held in its planes) through an RG32Uint image of its texels: the image, a block-linear scratch of
// the command buffer's (backing tag xrPlanesImageTag(k)), merged from the planes first; after the pass, the image split back
// into them. 0, or 1 a pass the raw path cannot render, 2 a draw the encoder refused (*why)
constexpr uint32_t kXrPlanesImageTag = 104;
// Past every fixed tag (raw_gpu.h kBackingTags: kFormatStandInTag's 16 are the last): a tag there is still a backing's own
// memory, only not in the device's last-backing cache
constexpr uint32_t kXrPlanesOverflowTag = 128;
// Image k's backing tag: 104 + k up to kFormatStandInTag (112), then 32 + k - 8 between the plane images' (raw_attachment_target.h
// kPlaneImageTag) and the turn copies' (raw_turned_pass.cpp kTurnTag 48), then kXrPlanesOverflowTag + k - 24 for as many as the
// pass writes
constexpr uint32_t xrPlanesImageTag(uint32_t k)
{
    return k < 8 ? kXrPlanesImageTag + k : k < 24 ? 32 + (k - 8) : kXrPlanesOverflowTag + (k - 24);
}
int mergeXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, uint32_t k, raw::Texture *&image, const char **why);
int splitXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, const raw::Texture &image, const char **why);
