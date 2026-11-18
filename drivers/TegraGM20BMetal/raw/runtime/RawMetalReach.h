// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_reach.h"

// A render pass's draws in its command buffer's plan, from its OpRender at `pass`, as its reach pass draws them again
// (raw_reach.h): each with its own state, applied through a cursor of its own. Nothing is read of the plan until a turned
// pass asks.
class PlanDraws final : public raw::DrawReplay {
public:
    PlanDraws(RMCommandBuffer *cb, size_t pass, size_t to, raw::CommandBuffer &commands)
        : cb_(cb), commands_(commands), from_(pass + 1), to_(to)
    {}

    bool any() const override;
    bool atFrameResolution() const override;
    bool replay(raw::RenderEncoder &e, const char **why) override;

private:
    RMCommandBuffer *cb_;
    raw::CommandBuffer &commands_;
    size_t from_, to_;  // to_: the command buffer's range end; the pass ends at its OpEnd before it
};

// A reach, or the work of a pass it bounds, that failed (`why`, its command buffer's `refusal`): the command buffer fails
// OutOfMemory where it had no memory for a target (`memory`: raw::PassReach's), else as the raw path's own draw the
// encoder refused (RawMetalPassErrors.h rmOwnDrawFails), whose line fail() then logs
void rmReachFails(RMCommandBuffer *cb, bool ahead, const char *why, bool memory, const raw::Refusal &refusal);
