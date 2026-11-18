// SPDX-License-Identifier: MIT
// The step at which the calling thread's last texture over an IOSurface stopped (Device::createSurfaceTexture), which the
// facade's one refusal line names (RawMetalDevice.mm raw:tex.surf.imp); null where none stopped since it was cleared
#pragma once

namespace raw {

inline thread_local const char *surfaceRefusal = nullptr;

}  // namespace raw
