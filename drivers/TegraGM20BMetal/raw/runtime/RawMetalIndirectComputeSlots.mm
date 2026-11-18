// SPDX-License-Identifier: MIT
// A buffer of dispatches' memory (RawMetalIndirectCompute.h): the blocks its commands' regions lie in, each command's
// slot and bind slot and their growth, and the commands of a buffer that inherits the pipeline written for one pipeline
#import "RawMetalIndirectCompute.h"

namespace {

// A block of `bytes` in `chunks`, a chunk added where the last has no room
std::shared_ptr<RMICBBlock> blockIn(std::vector<raw::Buffer> &chunks, uint64_t &used, uint32_t bytes)
{
    constexpr uint64_t kChunk = 256 << 10;
    if (chunks.empty() || used + bytes > chunks.back().length) {
        raw::Buffer chunk;
        if (!gRM->raw->createBuffer(std::max<uint64_t>(kChunk, bytes), chunk, true))
            return nullptr;
        chunks.push_back(chunk);
        used = 0;
    }
    const raw::Buffer &chunk = chunks.back();
    auto block = std::make_shared<RMICBBlock>(RMICBBlock{chunk.cpu + used, chunk.va + used, bytes});
    used += (bytes + raw::icb::kComputeBlockAlign - 1) & ~uint64_t(raw::icb::kComputeBlockAlign - 1);
    return block;
}

// Command `c` at `index` into its slot and, where its buffer inherits the encoder's buffers, its bind slot
void writeCommand(uint32_t *slot, uint32_t slotWords, uint32_t *bind, uint32_t bindWords, NSUInteger index,
                  const RMICBDispatch &c)
{
    if (c.raw)
        raw::icb::dispatch(slot, uint32_t(index), c.words, slotWords);
    else
        raw::icb::computeEmpty(slot, slotWords);
    if (bind && (!c.raw || !raw::icb::binds(bind, c.binds.data(), uint32_t(c.binds.size()), c.strideWords.data(),
                                            uint32_t(c.strideWords.size()), c.roots.data(), uint32_t(c.roots.size()),
                                            bindWords)))
        raw::icb::bindsEmpty(bind, bindWords);
}

// `memory` of `count` slots of `words` made `want` words each where that is more; the old memory into `retired`
bool grown(raw::Buffer &memory, uint32_t &words, uint64_t want, uint64_t count, std::vector<raw::Buffer> &retired)
{
    if (want <= words)
        return true;
    raw::Buffer more;
    if (want > UINT32_MAX || !gRM->raw->createBuffer(count * want * 4, more, true))
        return false;
    retired.push_back(memory);
    memory = more;
    words = uint32_t(want);
    return true;
}

}  // namespace

std::shared_ptr<RMICBBlock> rmNewBlock(RMIndirectComputeBuffer *b, uint32_t bytes)
{
    return blockIn(b->_chunks, b->_chunkUsed, bytes);
}

uint32_t *rmComputeSlot(RMIndirectComputeBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * b->_slotWords;
}

uint32_t *rmBindSlot(RMIndirectComputeBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_binds.cpu) + index * b->_bindWords;
}

uint64_t rmSlotWords(const RMICBDispatch &c)
{
    return c.raw ? raw::icb::computeSlotWords(c.words.qmd.size()) : raw::icb::kComputeSlotWords;
}

uint64_t rmBindWords(const RMICBDispatch &c)
{
    return c.raw ? raw::icb::computeBindSlotWords(c.binds.size(), c.strideWords.size(), c.roots.size())
                 : raw::icb::kComputeBindSlotWords;
}

void rmWriteWords(RMIndirectComputeBuffer *b, NSUInteger index)
{
    writeCommand(rmComputeSlot(b, index), b->_slotWords, b->_inheritBuffers ? rmBindSlot(b, index) : nullptr,
                 b->_bindWords, index, b->_dispatches[index]);
}

bool rmFitSlots(RMIndirectComputeBuffer *b, uint64_t slotWords, uint64_t bindWords)
{
    const uint32_t slotsWere = b->_slotWords, bindsWere = b->_bindWords;
    const bool fit = grown(b->_raw, b->_slotWords, slotWords, b->_count, b->_retired) &&
                     (!b->_inheritBuffers || grown(b->_binds, b->_bindWords, bindWords, b->_count, b->_retired));
    if (b->_slotWords != slotsWere || b->_bindWords != bindsWere)
        for (NSUInteger i = 0; i < b->_count; i++)
            rmWriteWords(b, i);
    return fit;
}

RMICBVariant::~RMICBVariant()
{
    for (raw::Buffer &chunk : chunks)
        gRM->raw->destroyBuffer(chunk);
    if (slots.va)
        gRM->raw->destroyBuffer(slots);
    if (binds.va)
        gRM->raw->destroyBuffer(binds);
}

std::shared_ptr<RMICBVariant> rmPipelineVariant(RMIndirectComputeBuffer *b, RMComputeState *pipeline,
                                                const std::vector<std::shared_ptr<RMKernelSet>> &kernels,
                                                const std::vector<RMICBUnmade> &unmade, const char **why)
{
    auto v = std::make_shared<RMICBVariant>();
    v->generation = b->_generation;
    v->missing.assign(b->_count, RMICBUnmade{});
    const uint64_t count = b->_count;
    std::vector<RMICBDispatch> made(count);
    uint64_t slotWords = raw::icb::kComputeSlotWords, bindWords = raw::icb::kComputeBindSlotWords;
    for (NSUInteger i = 0; i < count; i++) {
        const RMICBDispatch &c = b->_dispatches[i];
        const RMKernelSet *k = i < kernels.size() ? kernels[i].get() : nullptr;
        if (!c.dispatched)
            continue;
        if (!k) {
            if (i < unmade.size())
                v->missing[i] = unmade[i];
            continue;
        }
        const raw::ComputeDispatch shape = raw::computeDispatch(c.grid, c.threads, c.perThread, pipeline->_threadLimit);
        const uint64_t bytes = rmCommandBytes(*k, shape);
        const std::shared_ptr<RMICBBlock> block =
            bytes <= UINT32_MAX ? blockIn(v->chunks, v->chunkUsed, uint32_t(bytes)) : nullptr;
        if (!block) {
            *why = "no memory for an indirect dispatch command's regions at the encoder's pipeline";
            return nullptr;
        }
        raw::ComputeEncoder::IcbBuffer buffers[31];
        if (!b->_inheritBuffers)
            rmCommandBuffers(c, buffers);
        RMICBDispatch &m = made[i];
        if (!rmCommandRegions(*k, shape, c, buffers, c.strides, *block, m.words, why) ||
            (b->_inheritBuffers && !rmCommandBinds(*k, shape, *block, m, why))) {
            v->missing[i] = RMICBUnmade{*why};
            continue;
        }
        m.words.barrier = c.barrier;
        m.raw = true;
        slotWords = std::max(slotWords, rmSlotWords(m));
        bindWords = std::max(bindWords, rmBindWords(m));
        v->bindSlots |= m.bindSlots;
        v->strideSlots |= m.strideSlots;
    }
    if (slotWords > UINT32_MAX || bindWords > UINT32_MAX ||
        !gRM->raw->createBuffer(count * slotWords * 4, v->slots, true) ||
        (b->_inheritBuffers && !gRM->raw->createBuffer(count * bindWords * 4, v->binds, true))) {
        *why = "no memory for an indirect command buffer's commands at the encoder's pipeline";
        return nullptr;
    }
    v->slotWords = uint32_t(slotWords);
    v->bindWords = uint32_t(bindWords);
    for (NSUInteger i = 0; i < count; i++)
        writeCommand(reinterpret_cast<uint32_t *>(v->slots.cpu) + i * slotWords, v->slotWords,
                     b->_inheritBuffers ? reinterpret_cast<uint32_t *>(v->binds.cpu) + i * bindWords : nullptr,
                     v->bindWords, i, made[i]);
    return v;
}
