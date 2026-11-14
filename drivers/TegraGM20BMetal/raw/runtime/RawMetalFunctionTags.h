// SPDX-License-Identifier: MIT
// A function's reflection as Metal answers it from its metallib's public metadata (raw_function_tags.h): no twin made
#pragma once
#import <Metal/Metal.h>

@class RMFunction;

NSDictionary<NSString *, MTLFunctionConstant *> *rmFunctionConstants(RMFunction *f);

// vertexAttributes (`stageInput` false) or stageInputAttributes into `out` (nil where Metal answers nil). False for a function
// made with constant values that has attributes: which of them its constants gate off is in its AIR, not its tags.
bool rmFunctionAttributes(RMFunction *f, bool stageInput, NSArray **out);
