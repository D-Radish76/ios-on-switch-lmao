// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "RawMetalPlanApply.h"

void encodePlannedState(RMCommandBuffer *cb, size_t at, raw::CommandBuffer &commands, raw::RenderEncoder *render,
                        raw::ComputeEncoder *compute, raw::StateCursor &cursor);
