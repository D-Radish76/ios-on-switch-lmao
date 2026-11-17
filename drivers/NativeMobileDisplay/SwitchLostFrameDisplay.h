// SPDX-License-Identifier: BSD-2-Clause-Patent
// A frame whose rendering the GPU driver killed (TEGRA_SCANOUT_SOURCE_LOST): the copy's target is never flipped to, and the
// swap retires unshown, as a frame the console holds back does; the screen keeps the frame before it.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void dropLostFrame(const OriginalLegacySwapRequest *request, int directEntry)
{
    ++gpuLostFrames;
    IOLog("SwitchDisplay: frame %llu not shown: its rendering was lost on the GPU (%llu so far)\n", gpuSequence,
          gpuLostFrames);
    if (directEntry == kNoBuffer)
        ring.release();
    releaseIdleDirectReads();
    if (latchRetire && request) {
        retireUnshown(seqOf(request));
    } else if (!latchRetire && !gpuRetired && request && pendingSwapRequest() == request) {
        retireSwap(request, true);
    }
    gpuRetired = false;
    transferred = false;
    kickSwapQueue();
}
