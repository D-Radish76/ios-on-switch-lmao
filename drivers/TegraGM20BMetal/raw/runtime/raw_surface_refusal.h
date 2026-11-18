// SPDX-License-Identifier: MIT
// The step at which the calling thread's last texture over an IOSurface stopped (Device::createSurfaceTexture), which the
// facade's one refusal line names (RawMetalDevice.mm raw:tex.surf.imp); null where none stopped since it was cleared
#pragma once

namespace raw {

inline thread_local const char *surfaceRefusal = nullptr;

// A texel buffer over a surface the kernel laid out block-linear, whose bytes an M4 Max reads in order: a texture over a
// surface Metal makes that the raw path holds no form of (RawMetalSurfaceTextures.mm raw:tex.surf.form)
inline constexpr char kTiledTexelBuffer[] =
    "a texel buffer over a surface the kernel laid out block-linear, whose bytes an M4 Max reads in order";

}  // namespace raw
