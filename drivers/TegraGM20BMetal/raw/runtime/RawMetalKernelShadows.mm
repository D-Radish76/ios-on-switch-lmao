// SPDX-License-Identifier: MIT
// The range encode's compute encoders' images and stand-ins around their dispatches (raw_kernel_shadow.h)
#import "RawMetalRangeEncode.h"

#include <algorithm>
#include <vector>

// textures a kernel reaches through their images (raw_kernel_shadow.h): filled as their compute encoder begins,
// copied back as it ends, sooner where another resource of a dispatch reaches the same bytes. A load the
// command buffer's native view of a decoder's plane answers (readTexture) reads in place.
void RMRangeEncode::localShadows(std::vector<raw::KernelUse> &list)
{
    for (raw::KernelUse &u : list)
        if (u.shadowed && !u.writes) {
            const raw::Texture &x = ((__bridge RMTexture *)u.object)->_raw;
            if (const raw::Texture *native = rc_->readTexture(&x); native != &x) {
                u.shadowed = false;
                u.object = native;  // another resource over its bytes: an image written before is copied back first
            }
        }
}

void RMRangeEncode::shadowCopies(const std::vector<const void *> &back, const std::vector<const void *> &fill)
{
    raw::BlitEncoder *copy = rc_->blit();
    for (const void *o : back)
        rmMoveShadowImage(*copy, ((__bridge RMTexture *)o)->_raw, false);
    for (const void *o : fill)
        rmMoveShadowImage(*copy, ((__bridge RMTexture *)o)->_raw, true);
    copy->end();
}

void RMRangeEncode::beginKernelShadows(size_t encoderAt)
{
    encoderUses_.clear();
    for (size_t at = encoderAt; at < cb_->_plan.encoderEnd(encoderAt); ++at) {
        if (cb_->_plan.operation(at).op != OpDispatch || !cb_->_plan.dispatch(at).surfaceSlots)
            continue;
        kernelUses(cb_, at, dispatchUses_);
        localShadows(dispatchUses_);
        encoderUses_.insert(encoderUses_.end(), dispatchUses_.begin(), dispatchUses_.end());
    }
    dispatchUses_.clear();  // a dispatch of an encoder no use is tracked in brings none of these to the planner
    kernelShadows_.begin(encoderUses_.data(), encoderUses_.size());
    encoderShadows_ = !encoderUses_.empty();
    encoderTracked_ = std::any_of(encoderUses_.begin(), encoderUses_.end(), [](const raw::KernelUse &u) {
        return u.shadowed;
    }) || standInsGoStale(encoderAt);
    std::vector<const void *> fill;
    for (const raw::KernelUse &u : encoderUses_)
        if (u.shadowed && std::find(fill.begin(), fill.end(), u.object) == fill.end())
            fill.push_back(u.object);
    if (!fill.empty())
        shadowCopies({}, fill);
}

// the images written back, and every pool image of a texture a draw samples refreshed again before that draw: a
// kernel may have written its rows through another resource
void RMRangeEncode::endKernelShadows()
{
    const std::vector<const void *> back = kernelShadows_.end();
    if (!back.empty())
        shadowCopies(back, {});
    fresh_.clear();
    encoderShadows_ = encoderTracked_ = false;
}

// Whether a dispatch of the encoder at `encoderAt` writes texels a stand-in it reads is drawn from: only then does a dispatch
// look at what the ones before it wrote (an encoder reading none, or writing none of their bytes, never does)
bool RMRangeEncode::standInsGoStale(size_t encoderAt)
{
    std::vector<raw::ByteSpan> read;
    for (const raw::KernelUse &u : encoderUses_)
        if (u.derived)
            read.push_back(u.span);
    if (read.empty())
        return false;
    std::vector<raw::KernelUse> uses;
    for (size_t at = encoderAt; at < cb_->_plan.encoderEnd(encoderAt); ++at) {
        if (cb_->_plan.operation(at).op != OpDispatch)
            continue;
        kernelUses(cb_, at, uses);
        for (const raw::KernelUse &u : uses)
            if (u.writes && std::any_of(read.begin(), read.end(), [&](const raw::ByteSpan &r) {
                    return r.overlaps(u.span);
                }))
                return true;
    }
    return false;
}
