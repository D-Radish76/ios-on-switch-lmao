// SPDX-License-Identifier: MIT
// A YCbCr texture whose plane rows run past its surface's allocation (raw_ycbcr.h YcbcrPlan::window), which Metal reads as 0
// there: its planes are read through a GPU VA window of their own, the surface import's pages bound again at its start (the
// same BO, no second BoImport) and the rest sparse (the GMMU answers a read with 0 and drops a write, with no fault), in one
// VmBind (Kernel::bindWindow). A job names only the bound pages (Texture::storage, Window::pages): the kernel refuses a
// use no one binding holds.
#include "raw_lock.h"
#include "raw_gpu.h"

#include <algorithm>

#include "raw_gpu_internal.h"

namespace raw {

bool Device::openSparseWindow(Texture &t, uint64_t end)
{
    Import *import = t.onePlane ? t.import : t.luma ? t.luma->import : nullptr;
    if (!import || import->kernelTiled() || !k_.sparse())
        return false;
    const uint64_t page = k_.info().page_size, pages = import->bo.bytes, into = import->bo.va - import->bo.vaBase;
    const uint64_t bytes = (into + end + page - 1) & ~(page - 1);
    if (bytes <= pages)
        return true;
    const uint64_t va = k_.reserveVa(bytes);
    if (!va)
        return false;
    if (!k_.bindWindow(import->bo, va, import->kind, bytes)) {
        k_.unbind(va, bytes);
        k_.releaseVa(va, bytes);
        return false;
    }
    t.window = {va, bytes, pages};
    // a place past the surface's base, in the window (each plane's import binds the same pages, a surface of ID 0 anew)
    const auto moved = [&](uint64_t at, const Import &of) {
        return va + into + (at - of.bo.va);
    };
    if (t.onePlane) {
        t.planes.luma = moved(t.planes.luma, *import);
        return true;
    }
    std::lock_guard<raw::Mutex> l(lock_);
    for (Texture *plane : {t.luma, t.chroma}) {
        if (!plane)
            continue;
        plane->va = moved(plane->va, *plane->import);
        plane->storage.va = plane->va;
        plane->storage.bytes = std::min(plane->storage.bytes, va + pages - plane->va);
        if (!plane->sampled)
            writeTic(*plane);
    }
    return true;
}

void Device::closeSparseWindow(Texture &t)
{
    if (!t.window.bytes)
        return;
    k_.unbind(t.window.va, t.window.bytes);
    k_.releaseVa(t.window.va, t.window.bytes);
    t.window = {};
}

}  // namespace raw
