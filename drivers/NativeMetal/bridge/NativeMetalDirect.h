// SPDX-License-Identifier: MIT
// The raw GM20B path's direct compile of a render state (NativeMetalDirect.rs): what m2v's translator makes of the state for
// each of the pack generator's probe draws, with no pipeline state, command buffer or capture.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeMetalObjects.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_direct_abi.h"
#include "NativeMetalBlend.h"

#ifdef __cplusplus
extern "C" {
#endif

// `descriptor`'s state (its stages' linked functions included) as the translator makes it for the probe's draws, handed to
// `each` (valid during the call). `barrier`: compiled as on a device without the pixel interlock; `textureFormat` and
// `textureSamples`: the MTLPixelFormat and sample count of the probe's textures (0: BGRA8Unorm, 1); `binds` (null: none):
// what the probe binds besides. False with nmb_last_error: refused, or `each`'s false
bool nmb_render_pipeline_direct(NmbDevice *device, const NmbRenderPipelineDescriptor *descriptor, bool barrier,
                                uint64_t textureFormat, uint32_t textureSamples, const NmbDirectBinds *binds,
                                bool (*each)(const NmbDirectState *state, void *ctx), void *ctx);

// `descriptor`'s compute state as the translator makes it for a dispatch of `dispatch`'s threadgroup shape and bindings, handed
// to `each` (valid during the call). False with nmb_last_error: refused, or `each`'s false
bool nmb_compute_pipeline_direct(NmbDevice *device, const struct NmbComputePipelineDescriptor *descriptor,
                                 const NmbDirectDispatch *dispatch,
                                 bool (*each)(const NmbDirectKernel *kernel, void *ctx), void *ctx);

// The direct compile with no m2v runtime (NativeMetalOffline.rs): libraries and functions the translator makes alone, and
// states and kernels of them made on the build's compile profile's devices, every Vulkan handle they hand over 0.
typedef struct NmbOfflineLibrary NmbOfflineLibrary;
typedef struct NmbOfflineFunction NmbOfflineFunction;

// As nmb_library_new's metallib, nmb_function_new's and nmb_function_new_specialized's functions
NmbOfflineLibrary *nmb_offline_library_new(const void *bytes, size_t length);
void nmb_offline_library_release(NmbOfflineLibrary *);
NmbOfflineFunction *nmb_offline_function_new(const NmbOfflineLibrary *, const char *name);
NmbOfflineFunction *nmb_offline_function_new_specialized(const NmbOfflineLibrary *, const char *name,
                                                         const char *specialized_name,
                                                         const struct NmbConstant *constants, uint32_t count);
void nmb_offline_function_release(NmbOfflineFunction *);
uint64_t nmb_offline_function_type(const NmbOfflineFunction *);
// vertexAttributes, as nmb_function_attribute_count and nmb_function_attribute answer it
int64_t nmb_offline_function_vertex_attribute_count(const NmbOfflineFunction *);
size_t nmb_offline_function_vertex_attribute(const NmbOfflineFunction *, uint32_t at, char *out, size_t size,
                                             uint64_t info[5]);

typedef struct NmbOfflineRenderPipelineDescriptor {
    NMB_RENDER_PIPELINE_DESCRIPTOR_FIELDS(NmbOfflineFunction)
} NmbOfflineRenderPipelineDescriptor;

typedef struct NmbOfflineComputePipelineDescriptor {
    NMB_COMPUTE_PIPELINE_DESCRIPTOR_FIELDS(NmbOfflineFunction)
} NmbOfflineComputePipelineDescriptor;

// nmb_render_pipeline_direct's and nmb_compute_pipeline_direct's answers for offline functions
bool nmb_render_pipeline_offline(const NmbOfflineRenderPipelineDescriptor *descriptor, bool barrier,
                                 uint64_t textureFormat, uint32_t textureSamples, const NmbDirectBinds *binds,
                                 bool (*each)(const NmbDirectState *state, void *ctx), void *ctx);
bool nmb_compute_pipeline_offline(const NmbOfflineComputePipelineDescriptor *descriptor,
                                  const NmbDirectDispatch *dispatch,
                                  bool (*each)(const NmbDirectKernel *kernel, void *ctx), void *ctx);
// Whether the m2v runtime's own compile device compiles `descriptor`'s kernel, as its compute state creation does (false
// with nmb_last_error)
bool nmb_offline_compute_pipeline_compiles(const NmbOfflineComputePipelineDescriptor *descriptor);
// The answers the runtime's compute prepare gives `descriptor`'s state (its native compute artifact's metal_info), into
// `facts`: maxTotalThreadsPerThreadgroup, threadExecutionWidth, staticThreadgroupMemoryLength (false with nmb_last_error)
bool nmb_offline_compute_pipeline_facts(const NmbOfflineComputePipelineDescriptor *descriptor, uint32_t facts[3]);

// The build's compile profile held to `device`'s runtime (false: they differ, nmb_last_error says how), and the profile of
// `device`'s runtime as JSON into `out` (`length` bytes, unterminated): its length, 0 on failure
bool nmb_compile_profile_check(NmbDevice *device);
size_t nmb_compile_profile_freeze(NmbDevice *device, char *out, size_t length);

// Whether a heap array of class `heapClass` (metal2vulkan heap_class_code, a record's PackHeapArray) holds a texture of
// `facts`; the flags of its handle-table entry (metal2vulkan handle_entry_flags); a sampler's SamplerWord
bool switch_nmb_heap_holds(uint32_t heapClass, const NmbHeapTextureFacts *facts);
uint32_t switch_nmb_heap_texture_flags(const NmbHeapTextureFacts *facts);
uint32_t switch_nmb_sampler_word(const struct NmbSamplerDescriptor *descriptor);

#ifdef __cplusplus
}
#endif
