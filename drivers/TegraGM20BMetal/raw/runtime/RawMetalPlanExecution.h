// SPDX-License-Identifier: MIT
#pragma once
#include "RawMetalPlan.h"
#include "raw_execution_plan.h"

// The tape as ranges: each encoder native, each event its own. A command outside every encoder is one recorded on an
// encoder that had ended (Metal's validation error), which its commit moves inside that encoder (forEachLateRun,
// RMCommandPlan::joinEnded) before the tape is planned again
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

// Each run [from, to) of commands outside every encoder, as visit(encoder, from, to): `encoder` the begin of the
// encoder whose end the run follows, SIZE_MAX where an event (or nothing) precedes it. A visit that answers false stops
// the walk, as does an encoder that does not end inside the tape.
template <typename Visit> void forEachLateRun(const RMCommandPlan &commands, Visit visit)
{
    const auto boundary = [&](size_t at) {
        const Op op = commands.operation(at).op;
        return RMCommandPlan::isEncoderBegin(op) || op == OpSignalEvent || op == OpWaitEvent;
    };
    size_t encoder = SIZE_MAX;
    for (size_t at = 0; at < commands.size();) {
        if (RMCommandPlan::isEncoderBegin(commands.operation(at).op)) {
            const size_t end = commands.encoderEnd(at);
            if (end <= at || end > commands.size())
                return;
            encoder = at;
            at = end;
        } else if (boundary(at)) {
            encoder = SIZE_MAX;
            ++at;
        } else {
            size_t to = at + 1;
            while (to < commands.size() && !boundary(to))
                ++to;
            if (!visit(encoder, at, to))
                return;
            encoder = SIZE_MAX;
            at = to;
        }
    }
}

// Whether an encoder that begins with `encoder` records `op`, one of the tape's operations other than a begin, an end
// or an event
inline bool recordedBy(Op encoder, Op op)
{
    switch (op) {
    case OpUpdateFence:
    case OpWaitFence:
        return true;
    case OpBarrier:
    case OpUse:
    case OpVisibility:
        return encoder == OpRender;
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer:
    case OpMipmaps:
    case OpFillBuffer:
    case OpFillTexture:
    case OpResetCommands:
    case OpCopyCommands:
        return encoder == OpBlit;
    case OpKernelUse:
    case OpKernelBarrier:
    case OpDispatch:
    case OpPredicate:
    case OpExecuteKernels:
        return encoder == OpCompute;
    default:
        return encoder == OpRender && isPlanDraw(op);
    }
}
