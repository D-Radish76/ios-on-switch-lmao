// SPDX-License-Identifier: MIT
#pragma once
#include "RawMetalPlan.h"
#include "raw_execution_plan.h"

// The tape as ranges: each encoder native, each event its own. A command outside every encoder is one recorded on an
// encoder that had ended: Metal's validation error (an M4 Max with validation off runs it in that encoder:
// c38-audit/m2v-cut2/probe)
inline RMPlanError makeExecutionPlan(const RMCommandPlan &commands, raw::ExecutionPlan &result)
{
    const auto error = commands.recordingError();
    if (error != RMPlanError::None)
        return error;
    for (size_t at = 0; at < commands.size();) {
        const auto op = commands.operation(at).op;
        if (RMCommandPlan::isEncoderBegin(op)) {
            const auto &begin = commands.begin(at);
            if (begin.end <= at || begin.end > commands.size())
                return RMPlanError::UnclosedEncoder;
            result.append(at, begin.end, raw::ExecutionKind::native);
            at = begin.end;
        } else if (op == OpSignalEvent || op == OpWaitEvent) {
            result.append(at, at + 1, op == OpSignalEvent ? raw::ExecutionKind::signal : raw::ExecutionKind::wait);
            ++at;
        } else {
            return RMPlanError::CommandAfterEnd;
        }
    }
    return RMPlanError::None;
}
