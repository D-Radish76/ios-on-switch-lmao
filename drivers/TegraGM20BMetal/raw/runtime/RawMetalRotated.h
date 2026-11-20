// SPDX-License-Identifier: MIT
// Textures made with Metal's rotation SPI over a surface plane, and passes into them (raw_rotation.h, raw_rotated_pass.h)
#pragma once
#import "RawMetalInternal.h"
#include "raw_rotated_pass.h"

// The descriptor's MTLTextureRotation, 0 where it names none
NSUInteger descriptorRotation(MTLTextureDescriptor *d);

// A texture `d` makes over plane `plane` of `s` that Metal's rotation SPI turns, of a shape the raw path draws into (`out`: its
// rotation and frame); false for any other
bool rotatedShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::Rotation &out);
// Whether Metal makes `d`'s turned texture over the plane at all (a 2D one of a framebuffer's format whose turned frame
// the plane holds), and its rotation and frame (`out`)
bool turnedFrame(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::Rotation &out);
// Such a texture over the plane's own memory, its rotation kept (Texture::rotation)
bool createRotated(RMTexture *t, MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, const raw::Rotation &r);
// `d` at its plane's extent, not turned
MTLTextureDescriptor *planeDescriptor(MTLTextureDescriptor *d, const raw::Rotation &r);
// Whether `d` asks a rotation of a texture over no surface plane, which Metal makes none of: its line logged
bool refuseTurnedOffSurface(MTLTextureDescriptor *d);

// Why the raw path does not draw a pass of attachments `pass` straight into its turned planes, None where it does or nothing
// is turned
raw::RotatedRefusal rotatedPassRefusal(const RMPass &pass);
// The rotation of the pass's turned colour attachments, 0 for none
uint8_t passTurn(const RMPass &pass);
