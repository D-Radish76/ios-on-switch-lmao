// SPDX-License-Identifier: MIT
// A function's reflection as Metal answers it from its metallib's public metadata (raw_function_tags.h): no twin made
#pragma once
#import <Metal/Metal.h>

@class RMFunction;

NSDictionary<NSString *, MTLFunctionConstant *> *rmFunctionConstants(RMFunction *f);

// vertexAttributes (`stageInput` false) or stageInputAttributes; nil where Metal answers nil
NSArray *rmFunctionAttributes(RMFunction *f, bool stageInput);
