// SPDX-License-Identifier: MIT
// Indirect command buffers (RawMetalIndirect.mm, RawMetalIndirectPatches.mm, RawMetalIndirectBlit.mm, raw_icb.h)
#pragma once
#import "RawMetalInternal.h"
#include <unordered_map>
#include "raw_icb.h"

// A command as the CPU encoded it, beside its words: the kinds and buffers its buffer holds (take:at:), which a range
// reads
struct RMICBCommand {
    enum Kind : uint8_t {
        None,
        Draw,
        Indexed,
        Patches
    } kind = None;

    uint8_t primitive = 0, indexType = 0;
    uint32_t first = 0, count = 0, instances = 0, baseInstance = 0;
    int32_t baseVertex = 0;
    uint64_t indexOffset = 0;
    RMBuffer *indices = nil;
    raw::icb::PatchDraw patches{};  // a patch command's words
    RMBuffer *factors = nil, *patchIndices = nil, *pointIndices = nil;
    // what the CPU encoded into its index that Metal's validation stops and Apple's driver crashes on, or an object of
    // another device: an execute that reaches it fails InvalidResource (rmCallInvalid), until a reset of it
    RMPred invalid = PredNone;
    const char *invalidWhy = nullptr;
};

// The kinds of a buffer's commands (RMIndirectCommandBuffer kinds): triangles and lines or points take different polygon
// offsets (raw::icb::polygonOffset), patches a tessellation state
enum RMICBKinds : uint8_t {
    RMICBTriangles = 1,
    RMICBOthers = 2,
    RMICBPatches = 4,
    RMICBIndexed = 8
};

@interface RMIndirectCommandBuffer : RMObject {
@public
    raw::Buffer _raw;  // the commands' words, _slotWords each
    NSUInteger _count;
    uint32_t _slotWords;  // raw::icb::kPatchSlotWords where its descriptor has patch commands, else kSlotWords
    bool _patches;        // its descriptor's command types hold drawPatches: or drawIndexedPatches: (and the device's)
    raw::Mutex _lock;     // commands are encoded from any thread
    std::vector<RMICBCommand> _commands;
    std::unordered_map<void *, uint32_t> _buffers;  // the buffers its commands read, and how many name each
    uint32_t _kinds[4];                             // its commands of each RMICBKinds bit
    // a blit encoder's reset or copy wrote its words on the GPU: an execute fetches them after the GPU's writes land
    std::atomic<bool> _gpuWritten;
    std::atomic<uint32_t> _invalid;  // its commands marked invalid (RMICBCommand::invalid)
}
// Command `index` is now `command` (its words already written); a command the CPU encodes over one marked invalid keeps
// the mark, which only a reset (a command of none) clears
- (void)take:(RMICBCommand)command at:(NSUInteger)index;
- (uint8_t)kinds;
- (std::vector<RMBuffer *>)buffers;
// The first of commands [first, first + count) marked invalid, its mark in `why`; PredNone: none
- (RMPred)invalidIn:(NSUInteger)first count:(NSUInteger)count why:(const char **)why;
@end

@interface RMIndirectRenderCommand : RMObject {
@public
    RMIndirectCommandBuffer *_buffer;
    NSUInteger _index;
}
- (void)reset;
// Its words emptied and its index marked invalid with `p` and `why`
- (void)invalidate:(RMPred)p why:(const char *)why;
@end

inline uint32_t *rmCommandSlot(RMIndirectCommandBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * b->_slotWords;
}

// The raw encode of OpExecuteCommands: each command drawn as the encoder's own draw
bool encodeCommands(raw::RenderEncoder *re, raw::CommandBuffer *rc, const RMPlanDraw &draw, const char **why);
// OpResetCommands and OpCopyCommands on the copy engine (`be`)
void encodeCommandsBlit(raw::CommandBuffer *rc, raw::BlitEncoder *be, const Cmd &c);
