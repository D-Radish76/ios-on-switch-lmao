// SPDX-License-Identifier: MIT
// Compute dispatches on raw::Device (raw_device_harness.h). A kernel placed while the device had no memory for its
// argument tables: every dispatch that meets them still missing, direct or indirect, is refused as Metal's
// out-of-memory error (raw::ComputeFailure, which RawMetalRangeStep.mm fails its command buffer with), and the first
// that meets them once there is memory binds them. An indirect command of a kernel reading argument buffers through
// the device's tables, or through a root table of its buffer slots, is written as a dispatch of it binds them; one that
// inherits the encoder's buffers has the bind macro write that table's entries and the residual words its record fills
// as the CPU writes them for a command of its own buffers, and an execute fetches commands at the slot sizes their
// buffer grew to. A record made under the address maps its key states (PackKernel::mapsStated) binds its sampler
// headers as made.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_compute_record.h"
#include "../raw/runtime/raw_icb_compute.h"
// the class headers' nv_push_compat.h and Mesa's util/macros.h spell MIN2 and MAX2 apart
#undef MIN2
#undef MAX2
#include "../raw/pack/mesa/mme_fermi.h"
#include "../raw/pack/mesa/mme_fermi_sim.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint32_t kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0));
constexpr uint8_t kConstexpr = 0x80;
constexpr uint32_t kKnown = 1u << 31, kRootPush = 0x700;
// TEXSAMP0 ADDRESS_U, _V and _P (clb197tex.h): all WRAP
constexpr uint32_t kAddress = 0x1ff, kWrap = 0;

// A kernel reading argument buffers through the device's handle tables: the root table's address at root words 8 and 9,
// the texture table's descriptor at set-0 word 0
raw::ComputeRecord tableReader()
{
    raw::ComputeRecord r;
    r.code.assign(64, 0);
    r.cbufs = 1;
    r.cbufType[0] = raw::CbufRoot;
    r.kernel.rootTableWord = 9;
    r.kernel.statusWord = 11;
    r.kernel.entries = 1;
    r.kernel.setBytes = 16;
    r.kernel.entry[0] = {0, raw::KernelTextureTable, 0, 0};
    return r;
}

// A kernel whose root table holds the dispatch's buffer slots ([[buffer(1)]] here); placeCompute keys it by its address
const raw::ComputeRecord &slotReader()
{
    static const raw::ComputeRecord slots = [] {
        raw::ComputeRecord r = tableReader();
        r.kernel.entries = 0;
        r.kernel.setBytes = 0;
        r.kernel.rootSlots = 1u << 1;
        return r;
    }();
    return slots;
}

// An execute's head and bind slots (raw_icb_compute.h computeBindHead, binds) as the channel runs them: its scratch
// writes, kIcbBind on Mesa's Fermi simulator, the 3D class's inline-to-memory writes into `memory`, and the QMD of
// each compute launch the words hold (`launched`, SEND_PCAS_A's QMD address >> 8)
struct BindRun {
    uint32_t scratch[SWITCH_GM20B_SCRATCH_WORDS] = {};
    std::map<uint64_t, uint32_t> memory;
    std::vector<uint32_t> arguments, launched;
    size_t consumed = 0;
    uint64_t out = 0;

    static bool isScratch(uint32_t m)
    {
        return m >= NVB197_SET_MME_SHADOW_SCRATCH(0) && m < NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_WORDS);
    }

    void method(uint32_t m, uint32_t v)
    {
        if (isScratch(m))
            scratch[(m - NVB197_SET_MME_SHADOW_SCRATCH(0)) / 4] = v;
        else if (m == NVB197_OFFSET_OUT_UPPER)
            out = (out & 0xffffffffull) | uint64_t(v & 0xff) << 32;
        else if (m == NVB197_OFFSET_OUT)
            out = (out & ~0xffffffffull) | v;
        else if (m == NVB197_LOAD_INLINE_DATA) {
            memory[out] = v;
            out += 4;
        }
    }

    static uint32_t load(void *opaque)
    {
        auto &r = *static_cast<BindRun *>(opaque);
        EXPECT_LT(r.consumed, r.arguments.size()) << "a word past the call's";
        return r.consumed < r.arguments.size() ? r.arguments[r.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t m)
    {
        auto &r = *static_cast<BindRun *>(opaque);
        return isScratch(m) ? r.scratch[(m - NVB197_SET_MME_SHADOW_SCRATCH(0)) / 4] : 0;
    }

    static void emit(void *opaque, uint16_t m, uint32_t v)
    {
        static_cast<BindRun *>(opaque)->method(m, v);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        return nullptr;
    }

    void run(const uint32_t *w, size_t n)
    {
        for (size_t i = 0; i < n;) {
            const uint32_t h = w[i++], op = h >> 29, count = h >> 16 & 0x1fff, subc = h >> 13 & 7, m = (h & 0xfff) << 2;
            if (op == 4)
                continue;
            ASSERT_LE(i + count, n);
            if (subc == raw::SUBC_3D && m == NVB197_CALL_MME_MACRO(raw::mme::kIcbBind)) {
                arguments.assign(w + i, w + i + count);
                consumed = 0;
                const raw::mme::Program &p = raw::mme::kIcbBindProgram;
                std::vector<mme_fermi_inst> decoded(p.size);
                mme_fermi_decode(decoded.data(), p.words, p.size);
                const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
                mme_fermi_sim_core(uint32_t(decoded.size()), decoded.data(), &ops, this);
                EXPECT_EQ(consumed, arguments.size());
            } else if (subc == raw::SUBC_3D && op == 1) {
                for (uint32_t k = 0; k < count; k++)
                    method(m + 4 * k, w[i + k]);
            } else if (subc == raw::SUBC_COMPUTE && op == 1 && m == NVB1C0_SEND_PCAS_A && count) {
                launched.push_back(w[i]);
            }
            i += count;
        }
    }

    // The scratch words the device's channel context leaves (raw_mme.h install), its zero page's among them
    void install(const raw::Device &dev)
    {
        std::vector<uint32_t> words(raw::mme::kContextWords);
        raw::Push p{words.data(), words.data() + words.size()};
        raw::mme::install(p, 0, raw::DeviceHarness::zeroPage(dev).va);
        run(words.data(), size_t(p.at - words.data()));
    }

    // An execute of the encoder's buffer `buffer` bound at Metal buffer index `slot`, through `bind`
    void execute(uint32_t slot, const raw::ComputeEncoder::IcbBuffer &buffer, const std::vector<uint32_t> &bind)
    {
        uint64_t va[31] = {};
        uint32_t size[31] = {}, strides[31] = {};
        va[slot] = buffer.va;
        size[slot] = buffer.size;
        uint32_t head[8 + 3 * 31];
        raw::Push p{head, head + std::size(head)};
        raw::icb::computeBindHead(p, va, size, strides, 1u << slot);
        run(head, size_t(p.at - head));
        run(bind.data(), bind.size());
    }
};

// A command's bind slot of `binds`, `strides` and `roots`, as long as computeBindSlotWords says
std::vector<uint32_t> bindSlot(const std::vector<raw::icb::ComputeBind> &binds,
                               const std::vector<raw::icb::ComputeStride> &strides,
                               const std::vector<raw::icb::ComputeRoot> &roots)
{
    std::vector<uint32_t> slot(raw::icb::computeBindSlotWords(binds.size(), strides.size(), roots.size()));
    EXPECT_TRUE(raw::icb::binds(slot.data(), binds.data(), uint32_t(binds.size()), strides.data(),
                                uint32_t(strides.size()), roots.data(), uint32_t(roots.size()), uint32_t(slot.size())));
    return slot;
}

class DeviceComputeTables : public harness::DeviceTest {
protected:
    // placeCompute keys a placed program by its record's address: each lives as long as the test
    raw::ComputeRecord plain, reading;
    raw::Buffer arguments;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createBuffer(64, arguments, true));
        for (raw::ComputeRecord *r : {&plain, &reading}) {
            r->code.assign(64, 0);
            r->cbufs = 1;
            r->cbufType[0] = raw::CbufRoot;
        }
        plain.keyString = "kernel reading no argument buffers";
        plain.kernel.keyHash = 0xc0;
        reading.keyString = "kernel reading argument buffers";
        reading.kernel.keyHash = 0xc1;
        reading.kernel.rootTableWord = 9;
        reading.kernel.statusWord = 11;
    }

    // One dispatch of `k`, `indirect` its grid the words in `arguments`, committed: "" or why it was refused, and what
    // the encoder says the refusal is
    std::string dispatch(const raw::ComputeState *k, bool indirect, raw::ComputeFailure &failure)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        cb->internalCompletion();
        std::string failed;
        {
            raw::ComputeEncoder e(*cb, dev);
            e.setState(k);
            const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
            const char *why = nullptr;
            const bool made =
                indirect ? e.dispatchIndirect(&arguments, 0, {1, 1, 1}, &why) : e.dispatch(groups, threads, &why);
            if (!made)
                failed = why ? why : "?";
            failure = e.failure();
            e.end();
        }
        uint32_t seqno = 0;
        if (!failed.empty() || !cb->commit(seqno)) {
            cb->abandon();
            return failed.empty() ? std::string("not committed") : failed;
        }
        return "";
    }
};

TEST_F(DeviceComputeTables, AnIndirectDispatchBindsTheTablesItsKernelWasPlacedWithout)
{
    raw::ComputeFailure failure = raw::ComputeFailure::Internal;
    ASSERT_EQ(dispatch(raw::placeCompute(dev, plain), false, failure), "")
        << "the command buffers' own control memory, made before the refusals";
    kernel_model::model.refuseControl = true;
    const raw::ComputeState *k = raw::placeCompute(dev, reading);
    ASSERT_NE(k, nullptr);
    EXPECT_TRUE(k->heapMissing);
    for (const bool indirect : {true, false}) {
        SCOPED_TRACE(indirect);
        failure = raw::ComputeFailure::Internal;
        EXPECT_EQ(dispatch(k, indirect, failure), "no memory for the tables a kernel reads argument buffers through");
        EXPECT_EQ(failure, raw::ComputeFailure::OutOfMemory);
    }
    kernel_model::model.refuseControl = false;
    EXPECT_EQ(dispatch(k, true, failure), "");
    EXPECT_FALSE(k->heapMissing);
    EXPECT_EQ(uint64_t(k->argumentPush[9]) << 32 | k->argumentPush[8], raw::DeviceHarness::tables(dev).rootTableVa());
}

// An indirect command's region of a kernel reading argument buffers through the device's handle tables: its root table
// names the device's root table and its set 0 the texture table, as a dispatch's do (raw_compute.cpp argumentRoot)
TEST_F(DeviceComputeTables, AnIndirectCommandOfAKernelReadingArgumentTablesIsWritten)
{
    static const raw::ComputeRecord reader = tableReader();
    const raw::ComputeState *k = raw::placeCompute(dev, reader);
    ASSERT_NE(k, nullptr);
    ASSERT_TRUE(raw::ComputeEncoder::icbArgumentTables(dev, *k));
    const raw::ComputeDispatch shape = raw::computeDispatch({1, 1, 1}, {1, 1, 1}, false);
    ASSERT_EQ(shape.count, 1u);
    std::vector<uint8_t> block(raw::icb::computeRegionBytes(k->rec->setBytes));
    raw::ComputeEncoder::IcbBuffer buffers[31] = {};
    const raw::StageIn stageIn;
    const char *why = nullptr;
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, shape.regions[0], shape.grid, shape.nominal, buffers, stageIn,
                                                block.data(), 0x100000, &why))
        << (why ? why : "?");
    const uint32_t *root = reinterpret_cast<const uint32_t *>(block.data() + raw::icb::kComputeQmdBytes);
    const uint32_t *set = root + raw::icb::kComputeRootBytes / 4;
    const raw::HeapTables &tables = raw::DeviceHarness::tables(dev);
    EXPECT_EQ(uint64_t(root[kRootPush / 4 + 9]) << 32 | root[kRootPush / 4 + 8], tables.rootTableVa());
    EXPECT_EQ(uint64_t(set[1]) << 32 | set[0], tables.textureTableVa());
    EXPECT_EQ(set[2], tables.textureTableBytes());
}

// A kernel whose root table holds the dispatch's buffer slots ([[buffer(1)]] here): an indirect command's region holds
// that table of the command's own buffers past its set 0, as a dispatch's upload holds it (raw_compute.cpp
// argumentRoot)
TEST_F(DeviceComputeTables, AnIndirectCommandOfAKernelReadingItsBufferSlotsCarriesTheirTable)
{
    const raw::ComputeState *k = raw::placeCompute(dev, slotReader());
    ASSERT_NE(k, nullptr);
    const uint32_t tableAt = raw::icb::computeRegionBytes(0);
    ASSERT_EQ(raw::ComputeEncoder::icbRegionBytes(*k->rec), tableAt + raw::icb::kComputeBlockAlign);
    const raw::ComputeDispatch shape = raw::computeDispatch({1, 1, 1}, {1, 1, 1}, false);
    std::vector<uint8_t> block(raw::ComputeEncoder::icbRegionBytes(*k->rec));
    raw::ComputeEncoder::IcbBuffer buffers[31] = {};
    buffers[1] = {arguments.va + 16, 48};
    const raw::StageIn stageIn;
    const char *why = nullptr;
    constexpr uint64_t kVa = 0x100000;
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, shape.regions[0], shape.grid, shape.nominal, buffers, stageIn,
                                                block.data(), kVa, &why))
        << (why ? why : "?");
    const uint32_t *root = reinterpret_cast<const uint32_t *>(block.data() + raw::icb::kComputeQmdBytes);
    EXPECT_EQ(uint64_t(root[kRootPush / 4 + 9]) << 32 | root[kRootPush / 4 + 8], kVa + tableAt);
    const auto *entry = reinterpret_cast<const raw::RootEntry *>(block.data() + tableAt);
    EXPECT_EQ(entry[raw::kFirstSlotRoot + 1].host, arguments.va + 16);
    EXPECT_EQ(entry[raw::kFirstSlotRoot + 1].size, 48u);
    EXPECT_EQ(entry[raw::kFirstSlotRoot].host, 0u) << "a slot outside the kernel's is the null entry";
    std::vector<raw::icb::ComputeBind> binds;
    std::vector<raw::icb::ComputeStride> strides;
    std::vector<raw::icb::ComputeRoot> roots;
    ASSERT_TRUE(raw::ComputeEncoder::icbBinds(*k, kVa, binds, strides, roots, &why)) << (why ? why : "?");
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0].entry, kVa + tableAt + sizeof(raw::RootEntry) * (raw::kFirstSlotRoot + 1));
    EXPECT_EQ(roots[0].slot, 1u);
}

// The same kernel's command inheriting the encoder's buffers: the bind macro writes its table's entries from an
// execute's buffers, over the table its region holds of no buffer, into the table the CPU writes for a command that
// binds the same buffer itself, and back to the table of no buffer (the zero page's) at an execute that binds none
TEST_F(DeviceComputeTables, AnInheritingCommandsBindSlotWritesTheTableOfItsCommandOfItsOwnBuffers)
{
    const raw::ComputeState *k = raw::placeCompute(dev, slotReader());
    ASSERT_NE(k, nullptr);
    const uint32_t tableAt = raw::icb::computeRegionBytes(0), entries = raw::drawRootEntries(k->rec->rootSlots);
    const raw::ComputeDispatch shape = raw::computeDispatch({1, 1, 1}, {1, 1, 1}, false);
    std::vector<uint8_t> own(raw::ComputeEncoder::icbRegionBytes(*k->rec)), inherited(own.size());
    raw::ComputeEncoder::IcbBuffer buffers[31] = {};
    buffers[1] = {arguments.va + 16, 48};
    const raw::ComputeEncoder::IcbBuffer none[31] = {};
    const raw::StageIn stageIn;
    const char *why = nullptr;
    constexpr uint64_t kVa = 0x100000;
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, shape.regions[0], shape.grid, shape.nominal, buffers, stageIn,
                                                own.data(), kVa, &why))
        << (why ? why : "?");
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, shape.regions[0], shape.grid, shape.nominal, none, stageIn,
                                                inherited.data(), kVa, &why))
        << (why ? why : "?");
    std::vector<raw::icb::ComputeBind> binds;
    std::vector<raw::icb::ComputeStride> strides;
    std::vector<raw::icb::ComputeRoot> roots;
    ASSERT_TRUE(raw::ComputeEncoder::icbBinds(*k, kVa, binds, strides, roots, &why)) << (why ? why : "?");
    const std::vector<uint32_t> slot = bindSlot(binds, strides, roots);
    BindRun run;
    run.install(dev);
    for (const bool bound : {true, false}) {
        run.execute(1, bound ? buffers[1] : raw::ComputeEncoder::IcbBuffer{}, slot);
        const std::vector<uint8_t> &cpu = bound ? own : inherited;
        for (uint32_t w = 0; w < entries * sizeof(raw::RootEntry) / 4; w++) {
            uint32_t want, held;
            memcpy(&want, cpu.data() + tableAt + 4 * w, 4);
            memcpy(&held, inherited.data() + tableAt + 4 * w, 4);
            const auto written = run.memory.find(kVa + tableAt + 4 * w);
            EXPECT_EQ(written != run.memory.end() ? written->second : held, want) << "word " << w << " " << bound;
        }
    }
}

// A kernel whose record fills the root push word its buffer's residual goes to (word 12 here): a command inheriting the
// encoder's buffers holds the record's word there as a dispatch's root table does, its bind slot carries that word, and
// the bind macro writes it back at an execute of a base on 16 bytes after one off 16 left its residual there
TEST_F(DeviceComputeTables, AnInheritingCommandTakesBackTheResidualWordItsRecordFills)
{
    constexpr int kWord = 12;
    constexpr uint32_t kRecorded = 0x5eed;
    static const raw::ComputeRecord filled = [] {
        raw::ComputeRecord r;
        r.code.assign(64, 0);
        r.cbufs = 1;
        r.cbufType[0] = raw::CbufRoot;
        r.kernel.regionWord = -1;
        r.kernel.entries = 1;
        r.kernel.setBytes = 16;
        r.kernel.entry[0] = {0, raw::KernelBuffer, 2, uint32_t(kWord + 1) << 24};
        r.kernel.pushMask = 1u << kWord;
        r.kernel.pushWords[kWord] = kRecorded;
        return r;
    }();
    const raw::ComputeState *k = raw::placeCompute(dev, filled);
    ASSERT_NE(k, nullptr);
    const raw::ComputeDispatch shape = raw::computeDispatch({1, 1, 1}, {1, 1, 1}, false);
    std::vector<uint8_t> block(raw::ComputeEncoder::icbRegionBytes(*k->rec));
    const raw::ComputeEncoder::IcbBuffer none[31] = {};
    const char *why = nullptr;
    // a region's address as the device's allocator hands them out, above 2^32: the bind macro takes a residual word's
    // address of high word 0 for none
    constexpr uint64_t kVa = 0xc0'0010'0000ull;
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, shape.regions[0], shape.grid, shape.nominal, none,
                                                raw::StageIn{}, block.data(), kVa, &why))
        << (why ? why : "?");
    const uint64_t word = kVa + raw::icb::kComputeQmdBytes + kRootPush + 4 * kWord;
    uint32_t held;
    memcpy(&held, block.data() + raw::icb::kComputeQmdBytes + kRootPush + 4 * kWord, 4);
    EXPECT_EQ(held, kRecorded);
    std::vector<raw::icb::ComputeBind> binds;
    std::vector<raw::icb::ComputeStride> strides;
    std::vector<raw::icb::ComputeRoot> roots;
    ASSERT_TRUE(raw::ComputeEncoder::icbBinds(*k, kVa, binds, strides, roots, &why)) << (why ? why : "?");
    ASSERT_EQ(binds.size(), 1u);
    EXPECT_EQ(binds[0].residual, word);
    EXPECT_EQ(binds[0].recorded, kRecorded);
    const std::vector<uint32_t> slot = bindSlot(binds, strides, roots);
    BindRun run;
    for (const uint32_t offset : {8u, 0u, 4u, 0u}) {
        run.execute(2, {arguments.va + offset, 48}, slot);
        EXPECT_EQ(run.memory.at(word), offset ? offset : kRecorded) << offset;
    }
}

// An execute of a buffer whose slots grew for commands of nine launches and whose bind slots grew for twenty
// descriptors: the job fetches command 1's slot and bind slot at the grown sizes, so it holds command 1's launches and
// writes command 1's descriptors, none of command 0's
TEST_F(DeviceComputeTables, AnExecuteFetchesItsCommandsAtTheirBuffersGrownSlots)
{
    constexpr uint32_t kLaunches = 9, kDescriptors = 20;
    const uint32_t slotWords = uint32_t(raw::icb::computeSlotWords(kLaunches)),
                   bindWords = uint32_t(raw::icb::computeBindSlotWords(kDescriptors, 0, 0));
    ASSERT_GT(slotWords, raw::icb::kComputeSlotWords);
    ASSERT_GT(bindWords, raw::icb::kComputeBindSlotWords);
    // write-combined: the kernel model reads a submission's words from memory a BoAlloc made, as an ICB's are
    raw::Buffer slots, binds;
    ASSERT_TRUE(dev.createBuffer(2 * slotWords * 4, slots, true));
    ASSERT_TRUE(dev.createBuffer(2 * bindWords * 4, binds, true));
    const auto qmd = [](uint32_t command, uint32_t launch) {
        return 0x77'0000'0000ull + 0x10000 * command + 0x100 * launch;
    };
    const auto descriptor = [](uint32_t command, uint32_t d) {
        return 0x78'0000'0000ull + 0x10000 * command + 16 * d;
    };
    for (uint32_t i = 0; i < 2; i++) {
        raw::icb::ComputeCommand c;
        for (uint32_t r = 0; r < kLaunches; r++)
            c.qmd.push_back(qmd(i, r));
        raw::icb::dispatch(reinterpret_cast<uint32_t *>(slots.cpu) + i * slotWords, i, c, slotWords);
        std::vector<raw::icb::ComputeBind> own;
        for (uint32_t d = 0; d < kDescriptors; d++)
            own.push_back({descriptor(i, d), 0, 1, 0, 0});
        ASSERT_TRUE(raw::icb::binds(reinterpret_cast<uint32_t *>(binds.cpu) + i * bindWords, own.data(), kDescriptors,
                                    nullptr, 0, nullptr, 0, bindWords));
    }
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t before = kernel_model::model.jobs.size();
    {
        raw::ComputeEncoder e(*cb, dev);
        e.setBuffer(1, &arguments, 0);
        raw::ComputeEncoder::IcbExecute x = {&slots, 1, 1, 1, nullptr, 0, false};
        x.binds = &binds;
        x.bindSlots = 1u << 1;
        x.slotWords = slotWords;
        x.bindWords = bindWords;
        const char *why = nullptr;
        EXPECT_TRUE(e.executeSlots(x, &why)) << (why ? why : "");
        e.end();
    }
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    BindRun run;
    for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
        run.run(kernel_model::model.jobs[j].words.data(), kernel_model::model.jobs[j].words.size());
    std::vector<uint32_t> want;
    for (uint32_t r = 0; r < kLaunches; r++)
        want.push_back(uint32_t(qmd(1, r) >> 8));
    EXPECT_EQ(run.launched, want);
    for (uint32_t d = 0; d < kDescriptors; d++) {
        EXPECT_EQ(run.memory.count(descriptor(1, d)), 1u) << d;
        EXPECT_EQ(run.memory.count(descriptor(0, d)), 0u) << d;
    }
}

// A kernel whose bind offset spills past its push window into its argument table of bind offsets (raw_spill_table.h):
// an indirect command's region holds that table, the record's words with its own buffer's residual in its word, and a
// command inheriting the encoder's buffers has the bind macro write the residual into that word
TEST_F(DeviceComputeTables, AnIndirectCommandsRegionHoldsItsArgumentTableOfBindOffsets)
{
    static const raw::ComputeRecord spilling = [] {
        raw::ComputeRecord r = tableReader();
        r.kernel.rootTableWord = r.kernel.statusWord = 0;
        r.kernel.setBytes = 32;
        r.kernel.entries = 2;
        r.kernel.entry[0] = {0, raw::KernelBuffer, 1, uint32_t(raw::kRootPushWords + 2 + 1) << 24};
        r.kernel.entry[1] = {16, raw::KernelSpillTable, 0, 0};
        r.kernel.spillEntry = 2;
        r.kernel.spillWords = 4;
        r.spill = {7, 8, 0, 10};
        return r;
    }();
    const raw::ComputeState *k = raw::placeCompute(dev, spilling);
    ASSERT_NE(k, nullptr);
    const raw::ComputeEncoder::IcbRegionLayout layout = raw::ComputeEncoder::icbRegionLayout(*k->rec);
    ASSERT_EQ(layout.spill, raw::icb::computeRegionBytes(32));
    ASSERT_EQ(layout.bytes, layout.spill + raw::spillRange(4));
    const raw::ComputeDispatch shape = raw::computeDispatch({1, 1, 1}, {1, 1, 1}, false);
    std::vector<uint8_t> block(layout.bytes);
    raw::ComputeEncoder::IcbBuffer buffers[31] = {};
    buffers[1] = {arguments.va + 4, 60};
    const raw::StageIn stageIn;
    const char *why = nullptr;
    constexpr uint64_t kVa = 0x100000;
    ASSERT_TRUE(raw::ComputeEncoder::icbRegion(dev, *k, shape.regions[0], shape.grid, shape.nominal, buffers, stageIn,
                                                block.data(), kVa, &why))
        << (why ? why : "?");
    const uint32_t *table = reinterpret_cast<const uint32_t *>(block.data() + layout.spill);
    EXPECT_EQ(std::vector<uint32_t>(table, table + 4), (std::vector<uint32_t>{7, 8, 4, 10}));
    const uint32_t *set = reinterpret_cast<const uint32_t *>(block.data() + raw::icb::kComputeQmdBytes +
                                                             raw::icb::kComputeRootBytes);
    EXPECT_EQ(uint64_t(set[1]) << 32 | set[0], arguments.va) << "the buffer's base rounded down to 16 bytes";
    EXPECT_EQ(uint64_t(set[5]) << 32 | set[4], kVa + layout.spill);
    EXPECT_EQ(set[6], raw::spillRange(4));
    std::vector<raw::icb::ComputeBind> binds;
    std::vector<raw::icb::ComputeStride> strides;
    std::vector<raw::icb::ComputeRoot> roots;
    ASSERT_TRUE(raw::ComputeEncoder::icbBinds(*k, kVa, binds, strides, roots, &why)) << (why ? why : "?");
    ASSERT_EQ(binds.size(), 1u);
    EXPECT_EQ(binds[0].residual, kVa + layout.spill + 2 * 4);
}

// A kernel buffer entry with no residual word is a slot its program never addresses (raw_compute.h
// kernelBufferDescriptor): bound off 16 bytes, its descriptor starts at the base rounded down, nothing copied
TEST_F(DeviceComputeTables, ABufferSlotWithNoResidualWordBindsAtItsBaseRoundedDown)
{
    static const raw::ComputeRecord unaddressed = [] {
        raw::ComputeRecord r = tableReader();
        r.kernel.rootTableWord = r.kernel.statusWord = 0;
        r.kernel.regionWord = -1;
        r.kernel.qmd[kQmdCbufValid] |= 1;
        r.kernel.setBytes = 16;
        r.kernel.entries = 1;
        r.kernel.entry[0] = {0, raw::KernelBuffer, 1, 0};
        return r;
    }();
    const raw::ComputeState *k = raw::placeCompute(dev, unaddressed);
    ASSERT_NE(k, nullptr);
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    std::vector<uint32_t> set;
    {
        raw::ComputeEncoder e(*cb, dev);
        e.setState(k);
        e.setBuffer(1, &arguments, 4);
        const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
        const char *why = nullptr;
        EXPECT_TRUE(e.dispatch(groups, threads, &why)) << (why ? why : "");
        if (const uint32_t *words = raw::DeviceHarness::kernelSet(e))
            set.assign(words, words + 3);
        e.end();
    }
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
    ASSERT_EQ(set.size(), 3u);
    EXPECT_EQ(uint64_t(set[1]) << 32 | set[0], arguments.va);
    EXPECT_EQ(set[2], uint32_t((arguments.length + 15) & ~uint64_t(15))) << "from the rounded-down base to the end";
}

// A kernel buffer slot bound to nothing reads the zero page with range 0: no buffer load is bounds-checked
// (raw_nak_target.h)
TEST_F(DeviceComputeTables, ABufferSlotBoundToNothingReadsTheZeroPage)
{
    static const raw::ComputeRecord unbound = [] {
        raw::ComputeRecord r = tableReader();
        r.kernel.rootTableWord = r.kernel.statusWord = 0;
        r.kernel.regionWord = -1;
        r.kernel.qmd[kQmdCbufValid] |= 1;
        r.kernel.setBytes = 16;
        r.kernel.entries = 1;
        r.kernel.entry[0] = {0, raw::KernelBuffer, 1, 0};
        return r;
    }();
    const raw::ComputeState *k = raw::placeCompute(dev, unbound);
    ASSERT_NE(k, nullptr);
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    std::vector<uint32_t> set;
    {
        raw::ComputeEncoder e(*cb, dev);
        e.setState(k);
        const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
        const char *why = nullptr;
        EXPECT_TRUE(e.dispatch(groups, threads, &why)) << (why ? why : "");
        if (const uint32_t *words = raw::DeviceHarness::kernelSet(e))
            set.assign(words, words + 3);
        e.end();
    }
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
    ASSERT_EQ(set.size(), 3u);
    EXPECT_EQ(uint64_t(set[1]) << 32 | set[0], raw::DeviceHarness::zeroPage(dev).va);
    EXPECT_EQ(set[2], 0u);
}

class DeviceComputeStatedMaps : public harness::DeviceTest {
protected:
    raw::Texture converted;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(8, 8, kRGBA8, converted));
        converted.addressMap = 3;  // a 4:2:0 texture read as codes (raw_ycbcr.h YcbcrPlan::addressMap)
    }

    // A dispatch of a record sampling the converted texture through a constexpr sampler of a wrapping header: its set-0
    // word, or none where it was refused (`why`)
    std::vector<uint32_t> dispatched(const raw::ComputeRecord &r, std::string &why)
    {
        const raw::ComputeState *k = raw::placeCompute(dev, r);
        raw::CommandBuffer *cb = dev.begin();
        if (!k || !cb)
            return {};
        std::vector<uint32_t> out;
        {
            raw::ComputeEncoder e(*cb, dev);
            e.setState(k);
            e.setTexture(0, &converted);
            const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
            const char *refused = nullptr;
            if (!e.dispatch(groups, threads, &refused))
                why = refused ? refused : "?";
            else if (const uint32_t *set = raw::DeviceHarness::kernelSet(e))
                out.assign(set, set + 1);
            e.end();
        }
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return out;
    }

    static raw::ComputeRecord record(uint8_t mapsStated)
    {
        raw::ComputeRecord r;
        r.code.assign(64, 0);
        r.cbufType[0] = raw::CbufRoot;
        r.cbufs = 1;
        r.kernel.regionWord = -1;
        r.kernel.qmd[kQmdCbufValid] |= 1;
        r.kernel.setBytes = 4;
        r.kernel.entries = 2;
        r.kernel.entry[0] = {0, raw::KernelTexture, 0, 0};
        r.kernel.entry[1] = {0, raw::KernelSampler, kConstexpr, kKnown | 1u << 0};
        r.kernel.readOnlyEntries = 3;
        r.kernel.mapsStated = mapsStated;
        r.samplers = {{0x26000 | kWrap, 0x291, 0xfff000, 0, 0, 0, 0, 0}};
        return r;
    }
};

TEST_F(DeviceComputeStatedMaps, ARecordMadeUnderItsKeysMapsBindsItsHeaderAsMade)
{
    static const raw::ComputeRecord stated = record(1), unstated = record(0);
    std::string why;
    const std::vector<uint32_t> set = dispatched(stated, why);
    ASSERT_EQ(set.size(), 1u) << why;
    EXPECT_EQ(set[0] & 0xfffff, converted.tic);
    EXPECT_EQ(raw::DeviceHarness::samplerHeader(dev, set[0] >> 20)[0] & kAddress, kWrap);
    EXPECT_TRUE(dispatched(unstated, why).empty()) << "a record of no stated maps keeps the address-map rule";
    EXPECT_EQ(why, "a kernel sampler whose modes a converted texture's address map changes, compiled without that map");
}

}  // namespace
