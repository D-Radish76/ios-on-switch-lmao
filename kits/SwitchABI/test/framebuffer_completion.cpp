// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <OriginalLegacyFramebufferABI.h>

extern "C" void switch_framebuffer_dispatch_queued(IOMobileFramebufferLegacy *display,
                                                   const OriginalLegacySwapRequest *request, bool presented)
{
    display->completeQueuedSwap(request, presented);
}

extern "C" void switch_framebuffer_import_queued(IOMobileFramebufferLegacy *display,
                                                 const OriginalLegacySwapRequest *request, bool presented)
{
    display->IOMobileFramebufferLegacy::completeQueuedSwap(request, presented);
}

extern "C" void switch_framebuffer_dispatch_current(IOMobileFramebufferLegacy *display)
{
    display->completeCurrentSwap();
}
