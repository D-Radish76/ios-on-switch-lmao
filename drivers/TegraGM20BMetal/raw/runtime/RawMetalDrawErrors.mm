// SPDX-License-Identifier: MIT
// Draws raw::RenderEncoder refuses that go nowhere else: those Metal fails as well (each line names the Metal header or
// error that defines the failure), records breaking what their making guarantees (the driver's error) and draws the raw
// path has no form of yet; a refusal no case names is the driver's error too. Each logs its own line and fails its
// command buffer, which then submits nothing (RawMetalSubmit.mm finishes it with its error before anything could replay
// it elsewhere).
#import "RawMetalInternal.h"

// raw_direct.cpp turnedVariant sets PipelineTurnRefused where NVK's turned compile of the fragment stage failed, or
// where it differs from the generic compile in its cbuf types, early tests, interlock or sample shading. The second
// cannot happen: the turn (nvk_switch_turn.c) adds only cbuf 0 reads, derivative clones and an interpolation at a
// sample as one at its offset; early tests and sample shading come from the SPIR-V (spirv_to_nir.c), the interlock mode
// from those, and cbuf 0 is always the root table, the others the descriptors the program reads
// (nvk_nir_lower_descriptors.c build_cbuf_map). So the draw is one of a pipeline that could not be compiled.
void rmTurnRefused(RMCommandBuffer *cb, uint64_t keyHash, uint32_t turn, const char *key)
{
    SWITCH_REFUSE_DETAIL("raw", "pass.turn_refused", keyHash,
                         "state %016llx into a plane of rotation %u: no turned fragment program (NVK failed its turned "
                         "compile, which differs from the generic in no field a record holds): "
                         "MTLCommandBufferErrorInternal",
                         (unsigned long long)keyHash, turn);
    failCommandBuffer(cb,
                      [NSString stringWithFormat:@"A state drawn into a turned drawable whose turned fragment program "
                                                 @"could not be compiled (state %016llx: %s)",
                                                 (unsigned long long)keyHash, key ?: "its key unknown here"]);
}

bool drawFailed(RMCommandBuffer *cb, const raw::Refusal &f, const char *why)
{
    MTLCommandBufferError code = MTLCommandBufferErrorInternal;
    switch (f.id) {
    case raw::RefTessellationDraw:
        SWITCH_REFUSE(
            "raw", "draw.tessellation",
            "%s (primitive %u, the state tessellates: %u): Metal's validation error (MTLRenderCommandEncoder.h: "
            "drawPatches: needs a postTessellationVertexFunction, drawPrimitives: a normal vertexFunction)",
            why, f.a, f.b);
        break;
    case raw::RefAmplificationCount:
        SWITCH_REFUSE(
            "raw", "draw.amplification_count",
            "%s (%u of %u): Metal's validation error (MTLRenderCommandEncoder.h "
            "setVertexAmplificationCount:viewMappings:, at most MTLRenderPipeline.h maxVertexAmplificationCount)",
            why, f.a, f.b);
        break;
    case raw::RefHeapMemory:
        SWITCH_REFUSE("raw", "draw.heap_memory", "%s: MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)", why);
        code = MTLCommandBufferErrorOutOfMemory;
        break;
    case raw::RefTessellationScale:
        SWITCH_REFUSE("raw", "draw.tessellation_scale",
                      "%s: Metal's validation error (MTLRenderCommandEncoder.h setTessellationFactorScale:, a normal "
                      "float above 0)",
                      why);
        break;
    case raw::RefControlPointOffset:
        SWITCH_REFUSE("raw", "draw.control_point_offset",
                      "%s (offset %u of %u-byte indices): Metal's validation error (MTLRenderCommandEncoder.h "
                      "drawIndexedPatches: controlPointIndexBufferOffset, a multiple of 4 bytes)",
                      why, f.a, f.b);
        break;
    case raw::RefIndirectArguments:
        SWITCH_REFUSE("raw", "draw.indirect_arguments",
                      "%s (offset %u of %u bytes): Metal's validation error (MTLRenderCommandEncoder.h "
                      "indirectBufferOffset, a multiple of 4 bytes with the arguments inside the buffer)",
                      why, f.a, f.b);
        break;
    case raw::RefIndirectIndexOffset:
        SWITCH_REFUSE("raw", "draw.indirect_index_offset",
                      "%s (offset %u, %u-byte indices): Metal's validation error (MTLRenderCommandEncoder.h "
                      "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:, an offset of a "
                      "multiple of 4 bytes)",
                      why, f.a, f.b);
        break;
    case raw::RefCommandsRange:
        SWITCH_REFUSE("raw", "draw.commands_range",
                      "%s (commands %u + %u of %u): Metal's validation error (MTLRenderCommandEncoder.h "
                      "executeCommandsInBuffer:withRange:, a range inside the indirect command buffer)",
                      why, f.a, f.b, f.c);
        break;
    // what the records' making guarantees (raw_record_parts.cpp assembleRecord, the bridge's placements, the variant a
    // draw takes): a record that breaks it is the driver's error
    case raw::RefStreamSlot:
        SWITCH_REFUSE("raw", "draw.stream_slot",
                      "%s (buffer %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a vertex descriptor's "
                      "buffers are 0 to 30 (MTLVertexDescriptor.h)",
                      why, f.a);
        break;
    case raw::RefClipControl:
        SWITCH_REFUSE("raw", "draw.clip_control",
                      "%s: MTLCommandBufferErrorInternal (MTLCommandBuffer.h); every record's registers set the clip "
                      "control (raw_emit.cpp)",
                      why);
        break;
    case raw::RefCommandsRangeBuffer:
        SWITCH_REFUSE("raw", "draw.commands_range_buffer",
                      "%s (offset %u of %u bytes): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the recording "
                      "runs no execute whose range buffer is so (raw:icb.range)",
                      why, f.a, f.b);
        break;
    case raw::RefPatchCommands:
        SWITCH_REFUSE("raw", "draw.patch_commands_set_address",
                      "%s: MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the control stage m2v synthesizes reads "
                      "set 0 through three storage buffers, which NVK maps to a constant buffer",
                      why);
        break;
    case raw::RefLayerDropped:
        SWITCH_REFUSE(
            "raw", "draw.layer_dropped",
            "%s (%u layers): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the bridge makes a one-layer "
            "record of a slice-choosing state (LAYER_DROPPED) only of a multisampled storage form, and no "
            "layered pass is multisampled (raw_layers.h)",
            why, f.a);
        break;
    case raw::RefTurnRefused:
        rmTurnRefused(cb, uint64_t(f.b) << 32 | f.a, f.c, nullptr);
        return true;
    case raw::RefUnnamedUniform:
        SWITCH_REFUSE("raw", "draw.ubo_unnamed",
                      "%s: MTLCommandBufferErrorInternal (MTLCommandBuffer.h); assembleRecord refuses such a record",
                      why);
        break;
    case raw::RefSpillTable:
        SWITCH_REFUSE("raw", "draw.spill_table",
                      "%s (stage %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a record's argument tables "
                      "come with it, and a state with them is offered no split",
                      why, f.a);
        break;
    case raw::RefViewports:
        SWITCH_REFUSE("raw", "draw.viewports",
                      "%s (%u viewports): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); records since pack v21 "
                      "name the viewport count word",
                      why, f.a);
        break;
    case raw::RefWrittenTexture:
        SWITCH_REFUSE("raw", "draw.written_texture",
                      "%s (slot %u, bound %u, stored %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a draw "
                      "takes the variant made for its written textures' formats",
                      why, f.a, f.b, f.c);
        break;
    case raw::RefWrittenPlanes:
        SWITCH_REFUSE("raw", "draw.written_planes",
                      "%s (slot %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the pass images every "
                      "two-plane texture its draws write",
                      why, f.a);
        break;
    // every texture made for sampling has its header from its making (a stand-in's too, where it has
    // MTLTextureUsageShaderRead): one a draw samples with none is memoryless or not made for reading; and a draw takes
    // the variant made for the two-plane textures it binds
    case raw::RefNoTic:
        SWITCH_REFUSE("raw", "draw.no_header",
                      "%s (slot %u, format %u): Metal's validation error (MTLTexture.h usage: a texture a function "
                      "samples has MTLTextureUsageShaderRead and is not memoryless), or a variant without a two-plane "
                      "texture's header pair",
                      why, f.a, f.b);
        break;
    case raw::RefTestsInvariant:
        SWITCH_REFUSE("raw", "draw.tests_variant",
                      "%s (%u, %u, %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a draw takes the variant "
                      "made for its depth/stencil tests (RawMetalCubic.mm drawnTests), a pass the hidden "
                      "depth/stencil attachment its recorded states test, and a Depth16Unorm its draws test in their "
                      "shader renders converted (RawMetalRangeForms.mm depth16Tested)",
                      why, f.a, f.b, f.c);
        break;
    case raw::RefDepthStencil:
        SWITCH_REFUSE("raw", "draw.depth_stencil",
                      "%s (format %u, tests %u, samples %u): Metal's validation error (an M4 Max's validation layer: "
                      "\"For stencil attachment, the renderPipelineState pixelFormat must be MTLPixelFormatInvalid, as "
                      "no texture is set\", \"the render pipeline's pixelFormat (MTLPixelFormatDepth32Float_Stencil8) "
                      "does not match the framebuffer's pixelFormat (MTLPixelFormatDepth32Float)\"; MTLRenderPass.h "
                      "attachments of one sample count)",
                      why, f.a, f.b, f.c);
        break;
    case raw::RefStorageSamples:
        SWITCH_REFUSE("raw", "draw.storage_samples",
                      "%s (attachment %u, format %u, %u samples): Metal's validation error (an M4 Max's validation "
                      "layer: \"For color attachment 0, the texture sample count (4) does not match the "
                      "renderPipelineState colorSampleCount (1)\"; MTLRenderPipeline.h rasterSampleCount)",
                      why, f.a, f.b, f.c);
        break;
    case raw::RefMatrixUniform:
        SWITCH_REFUSE("raw", "draw.matrix_uniform",
                      "%s (buffer %u, offset %u, %u bytes bound): Metal's validation error (an M4 Max's validation "
                      "layer: \"missing Buffer binding at index 0\", \"has space for 16 bytes, but argument has a "
                      "length(80)\", \"must be a multiple of 16 but was set to 2\"; MTLRenderCommandEncoder.h "
                      "setFragmentBuffer:offset:atIndex:)",
                      why, f.a, f.b, f.c);
        break;
    // a storage slot's attachment is of its state's colour format (RenderEncoder::prepare's format check), whose texel
    // size the pack's storage view has, and a storage form views compressed memory (storageReadsCompressed)
    case raw::RefStorageView:
        SWITCH_REFUSE("raw", "draw.storage_view",
                      "%s (attachment %u, format %u, width %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); "
                      "a storage form's view is of its attachment's texel size",
                      why, f.a, f.b, f.c);
        break;
    case raw::RefXrShadow:
        SWITCH_REFUSE("raw", "draw.xr_shadow",
                      "%s (attachment %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a draw takes a shadow "
                      "variant only in a pass that fills the colour's shadow (RawMetalCubic.mm rmXrShadowDraw, "
                      "RawMetalRangePass.mm rmXrShadowFill)",
                      why, f.a);
        break;
    case raw::RefSamplerHeader:
        SWITCH_REFUSE("raw", "draw.sampler_header",
                      "%s (sampler %u, address map %u, kind %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); "
                      "a range makes every opaque border's and address map's header its draws sample through before "
                      "them (RawMetalEncode.mm borderHeaders, conversions), a sampler's compare header is made with "
                      "it and a record names only the constexpr samplers placed with it",
                      why, f.a, f.b, f.c);
        break;
    case raw::RefRotated:
        SWITCH_REFUSE("raw", "draw.turned_memory",
                      "%s (rotation %u): MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h), no room in the shader "
                      "heap or local memory for the state's turned fragment program",
                      why, f.b);
        code = MTLCommandBufferErrorOutOfMemory;
        break;
    case raw::RefTestsMemory:
        SWITCH_REFUSE("raw", "draw.tests_memory", "%s: MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)", why);
        code = MTLCommandBufferErrorOutOfMemory;
        break;
    case raw::RefViewHeader:
        SWITCH_REFUSE(
            "raw", "draw.view_header",
            "%s (attachment %u, format %u): MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h), no texture "
            "header left for the view its fetch reads",
            why, f.a, f.b);
        code = MTLCommandBufferErrorOutOfMemory;
        break;
    // draws Metal takes that the raw path has no form of yet: never handed to m2v either
    case raw::RefStrideTwin:
        SWITCH_REFUSE("raw", "draw.stride_twin", "%s: the raw path has no form of it yet; the command buffer fails",
                      why);
        break;
    // the fetch barrier sits between BEGINs (clb197.h PIXEL_SHADER_BARRIER) and a patch's triangles come from one; the
    // pixel interlock orders only inside a program's own ticket section (clb197.h SET_PIXEL_SHADER_INTERLOCK_CONTROL),
    // which only a storage form has
    case raw::RefGeometry:
        SWITCH_REFUSE("raw", "draw.fetch_patches",
                      "%s (primitive %u, %u patches, %u instances; 0: the GPU's): MTLCommandBufferErrorInternal "
                      "(MTLCommandBuffer.h); a tessellated draw of a state that reads the colour it writes keeps "
                      "Metal's order inside a patch only in the state's storage form (the pixel interlock), which "
                      "this state has none of for this pass",
                      why, f.a, f.b, f.c);
        break;
    default:
        why = why ? why : "a draw the encoder refused";
        SWITCH_REFUSE("raw", "draw.unnamed",
                      "%s (refusal %u: %u, %u, %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); every draw "
                      "refusal names its ending here",
                      why, unsigned(f.id), f.a, f.b, f.c);
        break;
    }
    failCommandBuffer(cb, @(why), code);
    return true;
}
