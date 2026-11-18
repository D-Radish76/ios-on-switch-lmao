// SPDX-License-Identifier: MIT
// Indirect command buffers of dispatches (RawMetalIndirectCompute.mm, RawMetalIndirectComputeKernels.mm,
// RawMetalIndirectComputeSlots.mm, RawMetalIndirectExecute.mm, raw_icb_compute.h)
#pragma once
#import "RawMetalComputeState.h"
#import "RawMetalIndirect.h"
#include <map>
#include <memory>
#include "raw_compute.h"
#include "raw_icb_compute.h"

using RMKernelSet = raw::ComputeKernelSet<raw::ComputeState>;

// GPU memory a command's regions lie in. A blit's copy of a command shares its source's until either is encoded again.
struct RMICBBlock {
    uint8_t *cpu = nullptr;
    uint64_t va = 0;
    uint32_t bytes = 0;
};

// Why a command that dispatches launches nothing of its own (`why` null: it launches, or its grid is empty), and what
// that is to the command buffer whose execute reaches it
struct RMICBUnmade {
    const char *why = nullptr;
    raw::ComputeFailure failure = raw::ComputeFailure::Internal;
    RMPred pred = PredComputeKernel;  // what the execute that reaches it fails as
};

// A dispatch command as the CPU encoded it
struct RMICBDispatch {
    RMComputeState *pipeline = nil;
    RMBuffer *buffers[31] = {};
    uint64_t offsets[31] = {};
    std::array<uint32_t, 31> strides = raw::StageIn::unbound();  // setKernelBuffer:offset:attributeStride:atIndex:'s
    std::vector<std::pair<uint32_t, uint32_t>> memory;  // threadgroup memory lengths by index, as the encoder keys them
    std::array<uint64_t, 3> grid{};
    std::array<uint32_t, 3> threads{};
    bool dispatched = false, perThread = false, barrier = false, stageIn = false;
    MTLRegion stageInRegion{};
    std::shared_ptr<RMKernelSet> kernels;  // its own pipeline's at its shape
    std::shared_ptr<RMICBBlock> block;
    raw::icb::ComputeCommand words{};  // what its slot launches
    // a buffer that inherits the encoder's buffers: its descriptors', stage-in strides' and root table entries' binds
    // (its bind slot's words), the Metal buffer indices they read, and those whose bind stride it reads
    std::vector<raw::icb::ComputeBind> binds;
    std::vector<raw::icb::ComputeStride> strideWords;
    std::vector<raw::icb::ComputeRoot> roots;
    uint32_t bindSlots = 0, strideSlots = 0;
    bool raw = false;  // its slot launches it (a buffer that inherits no pipeline)
    RMICBUnmade unmade;
    // what the CPU encoded that Metal's validation stops and that has no form: an object of another device, or what its
    // buffer inherits; it launches nothing and an execute that reaches it fails InvalidResource, until a reset of it
    RMPred invalid = PredNone;
    const char *invalidWhy = nullptr;
};

// A buffer that inherits the pipeline: its commands written once for one pipeline of the encoder's, as they were at
// `generation`, executed by reference as the buffer's own words are; never changed once made (an execute in flight
// keeps it)
struct RMICBVariant {
    uint64_t generation = 0;
    raw::Buffer slots, binds;
    uint32_t slotWords = raw::icb::kComputeSlotWords, bindWords = raw::icb::kComputeBindSlotWords;
    std::vector<raw::Buffer> chunks;
    uint64_t chunkUsed = 0;
    uint32_t bindSlots = 0, strideSlots = 0;
    std::vector<RMICBUnmade> missing;  // each command's at the pipeline
    ~RMICBVariant();
};

// _raw holds each command's slot of _slotWords, raw::icb::computeSlotWords of the most a command launches
@interface RMIndirectComputeBuffer : RMIndirectCommandBuffer {
@public
    MTLIndirectCommandBufferDescriptor *_descriptor;
    bool _inheritPipeline, _inheritBuffers;
    std::vector<RMICBDispatch> _dispatches;
    std::vector<raw::Buffer> _chunks;  // the blocks' memory, freed with the buffer
    uint64_t _chunkUsed;
    raw::Buffer _binds;   // a buffer that inherits the encoder's buffers: each command's bind slot of _bindWords
    uint32_t _bindWords;  // raw::icb::computeBindSlotWords of the most a command binds
    // the slots and bind slots before they grew, which submitted executes and copies still name: freed with the buffer
    std::vector<raw::Buffer> _retired;
    uint64_t _generation;  // bumped by every change of a command
    std::map<RMComputeState *, std::shared_ptr<RMICBVariant>> _variants;  // a buffer that inherits the pipeline
    // every command's bindSlots and strideSlots, made again at an execute after a command changed
    uint32_t _bindSlots, _strideSlots;
    bool _bindsChanged;
}
@end

@interface RMIndirectComputeCommand : RMObject {
@public
    RMIndirectComputeBuffer *_buffer;
    NSUInteger _index;
}
@end

// A compute encoder's execute of a buffer of dispatches (OpExecuteKernels' object, retained by its command buffer)
@interface RMKernelExecute : NSObject {
@public
    RMIndirectComputeBuffer *_buffer;
    RMBuffer *_range;  // the execution range the GPU reads at _rangeOffset; nil: [_first, _first + _count)
    uint64_t _rangeOffset;
    RMComputeState *_pipeline;  // the encoder's, for a buffer that inherits it
    uint32_t _first, _count;
    // a buffer that inherits the pipeline: each command's kernels at the encoder's (a slot of none launches nothing)
    // and why one that dispatches has none, made where the buffer had no variant for it at `_kernelsGeneration`, and
    // the variant the execute runs
    std::vector<std::shared_ptr<RMKernelSet>> _kernels;
    std::vector<RMICBUnmade> _unmade;
    uint64_t _kernelsGeneration;
    std::shared_ptr<RMICBVariant> _variant;
}
@end

// RawMetalIndirectCompute.mm
id rmNewComputeCommandBuffer(id device, MTLIndirectCommandBufferDescriptor *d, NSUInteger count);
// Command `index`'s words written again from what the CPU encoded, under the buffer's lock
void rmWriteCommand(RMIndirectComputeBuffer *b, NSUInteger index);

// RawMetalIndirectComputeKernels.mm
// The kernels of `pipeline` for command `c`'s dispatch, made on the calling thread where no other dispatch of the shape
// made them, and `shape` its regions; null where it launches nothing: `unmade` says why, or nothing for an empty grid
std::shared_ptr<RMKernelSet> rmCommandKernels(RMComputeState *pipeline, const RMICBDispatch &c,
                                              raw::ComputeDispatch &shape, RMICBUnmade &unmade);
// Command `c`'s regions written into `block` (computeRegionBytes each) with `buffers` bound at `strides`, and the words
// that launch them
bool rmCommandRegions(const RMKernelSet &kernels, const raw::ComputeDispatch &shape, const RMICBDispatch &c,
                      const raw::ComputeEncoder::IcbBuffer (&buffers)[31], const std::array<uint32_t, 31> &strides,
                      const RMICBBlock &block, raw::icb::ComputeCommand &words, const char **why);
// The bytes of a command's regions: past UINT32_MAX, more than X1's 4 GB of memory holds
uint64_t rmCommandBytes(const RMKernelSet &kernels, const raw::ComputeDispatch &shape);
void rmCommandBuffers(const RMICBDispatch &c, raw::ComputeEncoder::IcbBuffer (&out)[31]);
// Command `c`'s binds of its regions in `block` into c.binds and its masks; false: refused (*why)
bool rmCommandBinds(const RMKernelSet &kernels, const raw::ComputeDispatch &shape, const RMICBBlock &block,
                    RMICBDispatch &c, const char **why);

// RawMetalIndirectComputeSlots.mm
// A block of `bytes` for a command's regions in `b`'s chunks; null: no memory for it
std::shared_ptr<RMICBBlock> rmNewBlock(RMIndirectComputeBuffer *b, uint32_t bytes);
// The slot and bind slot sizes command `c` needs
uint64_t rmSlotWords(const RMICBDispatch &c);
uint64_t rmBindWords(const RMICBDispatch &c);
uint32_t *rmComputeSlot(RMIndirectComputeBuffer *b, NSUInteger index);
// The bind slot of command `index` of a buffer that inherits the encoder's buffers
uint32_t *rmBindSlot(RMIndirectComputeBuffer *b, NSUInteger index);
// Command `index`'s slot and bind slot as the CPU keeps it (under the buffer's lock)
void rmWriteWords(RMIndirectComputeBuffer *b, NSUInteger index);
// `b`'s slots at least `slotWords` and its bind slots at least `bindWords` long (under its lock), each command written
// again where they grew; false: no memory for them
bool rmFitSlots(RMIndirectComputeBuffer *b, uint64_t slotWords, uint64_t bindWords);
// `b`'s commands (under its lock) for `pipeline` with `kernels` (each command's at it) and why each that dispatches has
// none (`unmade`), made now; null: no memory for them (*why)
std::shared_ptr<RMICBVariant> rmPipelineVariant(RMIndirectComputeBuffer *b, RMComputeState *pipeline,
                                                const std::vector<std::shared_ptr<RMKernelSet>> &kernels,
                                                const std::vector<RMICBUnmade> &unmade, const char **why);

// RawMetalCompute.mm: the pipeline the encoder has set
@interface RMComputeEncoder (State)
- (RMComputeState *)rmState;
@end

// RawMetalCompute.mm: a compute call or command that fails its command buffer with Metal's validation error, its
// out-of-memory error or the driver's own (`failure`), named by `pred` with `why` in the predicate's first line. A call
// that fails records nothing of itself.
void rmComputeFails(RMCommandBuffer *cb, raw::ComputeFailure failure, RMPred pred, uint64_t a, const char *why);
// RawMetalComputeAdmission.mm: whether a predication Start or EndDoWhile naming no buffer withholds the dispatches of
// the compute unit holding plan position `at`, whose encoders are `serial`
bool rmComputeUnitWithheld(RMCommandBuffer *cb, size_t at, bool serial);

// RawMetalIndirectExecute.mm: the raw encode of OpExecuteKernels, and a blit's reset or copy of a buffer of dispatches
bool encodeKernelCommands(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMKernelExecute *x, const char **why);
void encodeKernelCommandsBlit(raw::CommandBuffer *rc, raw::BlitEncoder *be, const Cmd &c);
