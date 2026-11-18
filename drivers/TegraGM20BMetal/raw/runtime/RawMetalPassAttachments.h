// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"

// The render pass attachments the raw path takes (RawMetalEncoder.mm renderCommandEncoderWithDescriptor:)

// The pass's depth/stencil texture: its depth attachment's, else its stencil attachment's
id<MTLTexture> rmZetaAttachment(MTLRenderPassDescriptor *pass);
// Whether store action `a` resolves
bool rmResolvesStore(MTLStoreAction a);
// What a store action keeps of the attachment itself: a resolve's as DontCare (MultisampleResolve) or Store
MTLStoreAction rmKeptStore(MTLStoreAction a);
raw::StencilSample rmStencilPick(MTLRenderPassDescriptor *pass);
// Whether the depth and stencil attachments are apart: two textures, or one at two levels or slices. An M4 Max loads, clears
// and stores each aspect in its own texture, level and slice, whatever the other's format (Depth32Float_Stencil8 beside a
// Stencil8, a Depth32Float beside a Depth32Float_Stencil8's stencil) and extent (the render area their smallest:
// raw/test/passformsoracle.mm zeta_two_*, pair_sizes, mixed_pairs). The pass renders a Depth32Float_Stencil8 of theirs
// (raw::PassDesc::separate), every layer of a layered pass from each one's own slice, each resolved aspect from its own
// texture after the pass stores it there. GM20B's zeta formats pair stencil only with a 24-bit or 32-bit float depth
// (SET_ZT_FORMAT), so a Depth16Unorm's depth goes through it converted by draws (raw::CommandBuffer::depthConvert), a layer
// a pass, a multisampled one's a sample at a time where the pack has those states; a multisampled one's depth neither
// loaded nor kept needs no conversion
bool rmSeparateZeta(MTLRenderPassDescriptor *pass);

enum class RMZetaForm : uint8_t {
    Raw,
    NotRaw,   // an attachment the raw path holds no zeta of
    Unbuilt,  // a form not built (*why)
};
// Whether the raw path renders the pass's depth/stencil attachments (raw::DepthStencilAttachment): one zeta texture
// (raw_zeta.h) as each aspect its format has (Stencil8 a stencil attachment alone), or a pair apart (rmSeparateZeta),
// each at a level and slice it holds, each aspect's resolve one the raw path draws. Called on a pass rmPassAsApple made
// and rmZetaAttachmentInvalid took.
RMZetaForm rmRawDepthStencil(MTLRenderPassDescriptor *pass, bool separate, const char **why);
// Level `level`, slice `slice` of zeta texture `t` as the image a pass renders or resolves into: `t` itself (level 0 of
// slice 0, or a memoryless one), else `part` made from the chain of t's levels owner; null: no such level
const raw::Texture *rmZetaPart(RMTexture *t, NSUInteger level, NSUInteger slice, raw::Texture &part);
// The texture header of that part, made once and freed with its owner's (raw::Device::attachmentTic); 0: none
uint32_t rmZetaPartTic(RMTexture *t, NSUInteger level, NSUInteger slice, const raw::Texture &part);
// A resolve the raw path draws after the pass (the pack's resolve state of its format, RawMetalEncode.mm): of a
// multisampled attachment into a level, slice or depth plane of a one-sample texture of its format the raw path
// renders to (the attachment's own target, raw_attachment_target.h), or a one-sample attachment's, which is a copy; a
// stand-in attachment's into a one-sample stand-in target (RawMetalRangeSeal.mm standInResolve)
bool rmRawResolve(RMCommandBuffer *cb, MTLRenderPassColorAttachmentDescriptor *a, RMTexture *from,
                  RMPass::Color &facts);
// A colour target of a format the GM20B renders none of, through a stand-in (raw_format_standin.h) of the pass's layers
// and its samples (raw::formatStandInSamples), a turned one's in the plane's shape and turned
// (RawMetalRangeForms.mm formatStandIns); a level, slice or 3D plane of it is taken as any target's part is
// (rawAttachmentPart)
bool rmFormatTarget(RMTexture *x);
// A view's whole first level, a block-linear 2D or 1D image of a colour target format that createPartView marks no target
// (one of several levels, or 1D): the pass binds it as it is, renderable
bool rmWholeViewTarget(RMTexture *x);
// Why the raw path renders no form of level `level`, slice `slice` (a 3D texture's plane) of raw texture `x` as a colour
// target, null where it does: a target as it is, a pitch one through a shadow, a stand-in, a part, a texture buffer's row
// (raw::texelBufferTarget) or a plane inside blocks of several (raw::planeInBlocks)
const char *rmColorTargetMissing(RMTexture *x, NSUInteger level, NSUInteger slice);
// ... why it binds no image of that part (`resolving`: one a resolve reads)
const char *rmColorPartMissing(RMTexture *x, NSUInteger level, NSUInteger slice, bool resolving);
// Why a pass into planar or YCbCr target `x` fails its command buffer, null where the raw path renders it. A planar target
// has no stand-in where it was made without render-target usage, which Metal's validation stops a pass into, or the device
// could not make one (raw:planar.target)
const char *rmPlanarTargetRefusal(RMTexture *x, MTLRenderPassColorAttachmentDescriptor *a);
