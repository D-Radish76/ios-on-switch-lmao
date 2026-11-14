// SPDX-License-Identifier: MIT
// Libraries and functions with no m2v runtime (bridge/NativeMetalOffline.rs): made by the translator alone from a
// metallib's file or bytes, for the pack generator (where probe_states.h and kernel_shapes.h make a corpus line's state,
// so a state's key is made as before) and the shader compiler daemon; and the direct compile of a descriptor of them.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <string>
#include "RawDirect.h"

// The device calls the line parsers make of a library: newLibraryWithURL:error: and newLibraryWithData:error:
@interface RawOfflineDevice : NSObject
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error;
@end

namespace offline {

// The direct compile's translation of descriptors of RawOfflineDevice's functions
raw::DirectTranslator translator();

// Whether the m2v runtime makes a compute state of `descriptor` (of RawOfflineDevice's functions): its own compile
// device compiles the kernel. False with `why`.
bool kernelMade(MTLComputePipelineDescriptor *descriptor, std::string &why);

// The answers the runtime's compute prepare gives `descriptor`'s state (of RawOfflineDevice's functions): its
// maxTotalThreadsPerThreadgroup, threadExecutionWidth and staticThreadgroupMemoryLength (raw_pack.h PackComputeFacts).
// False with `why`.
bool kernelFacts(MTLComputePipelineDescriptor *descriptor, uint32_t facts[3], std::string &why);

}  // namespace offline
