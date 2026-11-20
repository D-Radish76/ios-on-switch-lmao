// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.cpp on raw::Device over the kernel model: private chains, arrays and stored multisampled
// textures on compressed memory made off the render thread (TextureAsk::compressKind), and the move to plain memory
// before an m2v twin, every level, layer and sample copied by the copy engine. Then the clears of a pass over Core
// Animation's zoom window target (boot 21: 1792x1472 BGRA8) whose memoryless second colour only its draws see
// (raw_clear_box.h).
#include "raw_device_private_comp.h"
#include "../raw/runtime/raw_record.h"
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

using private_comp::copies;
using private_comp::DevicePrivateComp;
using private_comp::kBGRA8;
using private_comp::kH;
using private_comp::kW;

constexpr uint32_t kRGBA16F = 115, kTriangle = 3;

TEST_F(DevicePrivateComp, AChainTakesCompressedMemoryAndMovesEveryLevelToPlain)
{
    raw::Texture first, chain;
    compressedSecond(first, chain, [&](raw::Texture &t) {
        return dev.createMipTexture(256, 256, kBGRA8, 5, t, nullptr, raw::kCompKindC32);
    });
    EXPECT_TRUE(bound(raw::kCompKindC32));
    ASSERT_TRUE(chain.chain);
    EXPECT_EQ(chain.chain->va, chain.va);
    EXPECT_EQ(chain.cpu, nullptr) << "no CPU view of compressed memory";
    raw::Texture level;
    ASSERT_TRUE(raw::chainLevelImage(chain, 1, level));
    EXPECT_TRUE(level.comptags) << "level 1 is rendered with SET_COLOR_COMPRESSION, as level 0 is";
    ASSERT_TRUE(raw::chainLevelTarget(chain, 2, level));
    EXPECT_TRUE(level.comptags);
    const uint64_t compressed = chain.va;
    const uint32_t tic = chain.tic;
    const raw::fx::MipChain layout = chain.chain->layout;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(chain));
    EXPECT_FALSE(chain.comptags);
    EXPECT_FALSE(dev.privateCompressed(chain));
    EXPECT_EQ(chain.tic, tic) << "rewritten in place by the move's job";
    ASSERT_TRUE(chain.chain);
    EXPECT_EQ(chain.chain->va, chain.va);
    EXPECT_NE(chain.va, compressed);
    ASSERT_TRUE(raw::chainLevelImage(chain, 1, level));
    EXPECT_FALSE(level.comptags);
    const auto launched = copies(jobs);
    for (uint32_t level = 0; level < layout.levels; level++)
        EXPECT_NE(
            std::find(launched.begin(), launched.end(),
                      std::make_pair(compressed + layout.level[level].offset, chain.va + layout.level[level].offset)),
            launched.end())
            << "level " << level;
    dev.destroyTexture(chain);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AnArrayMovesEveryLayer)
{
    raw::Texture first, array;
    compressedSecond(first, array, [&](raw::Texture &t) {
        return dev.createArray(128, 128, 3, kBGRA8, raw::fx::kTexTwoDArray, t, 1, nullptr, raw::kCompKindC32);
    });
    const uint64_t compressed = array.va, stride = array.layerBytes;
    ASSERT_EQ(array.layers, 3u);
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(array));
    EXPECT_FALSE(array.comptags);
    const auto launched = copies(jobs);
    for (uint32_t layer = 0; layer < 3; layer++)
        EXPECT_NE(std::find(launched.begin(), launched.end(),
                            std::make_pair(compressed + layer * stride, array.va + layer * stride)),
                  launched.end())
            << "layer " << layer;
    dev.destroyTexture(array);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AStoredMultisampledTextureMovesAsItsSampleGrid)
{
    raw::Texture first, ms;
    const uint32_t kind = raw::compressedKind(4, 0, 4);
    compressedSecond(first, ms, [&](raw::Texture &t) {
        return dev.createStoredMultisample(64, 64, kBGRA8, 4, t, nullptr, kind);
    });
    EXPECT_TRUE(bound(kind)) << "C32_MS4_2CRA";
    const uint64_t compressed = ms.va;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(ms));
    EXPECT_FALSE(ms.comptags);
    EXPECT_EQ(ms.samples, 4u);
    const auto launched = copies(jobs);
    ASSERT_FALSE(launched.empty());
    EXPECT_EQ(launched.front(), std::make_pair(compressed, ms.va));
    dev.destroyTexture(ms);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AMovedTexturesCompressedMemoryStaysMadeWhenItGoes)
{
    raw::Texture first, t;
    compressedSecond(first, t, [&](raw::Texture &x) {
        return dev.createMipTexture(128, 128, kBGRA8, 3, x, nullptr, raw::kCompKindC32);
    });
    ASSERT_TRUE(dev.plainFromCompressed(t));
    const size_t freedBefore = kernel_model::model.freed.size(), ready = raw::DeviceHarness::compReady(dev);
    dev.destroyTexture(t);
    EXPECT_EQ(kernel_model::model.freed.size(), freedBefore);
    EXPECT_EQ(raw::DeviceHarness::compReady(dev), ready + 1) << "made for the next texture of its kind";
    dev.destroyTexture(first);
}

// Core Animation's zoom window target, made again at each app open and close: the memory a closed one held makes the
// next one compressed at its creation, with no BoAlloc
TEST_F(DevicePrivateComp, AClosedTargetsMemoryServesTheNextOfItsKind)
{
    raw::Texture first, opened;
    compressedSecond(first, opened, [&](raw::Texture &x) {
        return dev.createCompressedTexture(kW, kH, kBGRA8, raw::kCompKindC32, x) ||
               dev.createTexture(kW, kH, kBGRA8, x, false, true);
    });
    const uint64_t va = opened.va, handles = kernel_model::model.nextHandle;
    const size_t freed = kernel_model::model.freed.size();
    dev.destroyTexture(opened);
    EXPECT_EQ(kernel_model::model.freed.size(), freed);
    raw::Texture again;
    ASSERT_TRUE(dev.createCompressedTexture(kW, kH, kBGRA8, raw::kCompKindC32, again));
    EXPECT_EQ(again.va, va);
    EXPECT_EQ(kernel_model::model.nextHandle, handles) << "no BoAlloc";
    dev.destroyTexture(again);
    dev.destroyTexture(first);
}

// An ask refused for a shortage waits for memory given back (raw_comp.h CompQueue::refuse): memory that cannot serve it
// goes back when its texture goes, as before, so that ask may be made again
TEST_F(DevicePrivateComp, AClosedTargetsMemoryGoesBackWhereARefusedAskWantsMore)
{
    raw::Texture first, opened, larger;
    compressedSecond(first, opened, [&](raw::Texture &x) {
        return dev.createCompressedTexture(kW, kH, kBGRA8, raw::kCompKindC32, x) ||
               dev.createTexture(kW, kH, kBGRA8, x, false, true);
    });
    const uint64_t refused = dev.stats.compFailed.load();
    kernel_model::model.refuseAllocCount = 1;
    ASSERT_FALSE(dev.createCompressedTexture(2 * kW, kH, kBGRA8, raw::kCompKindC32, larger));
    for (int i = 0; i < 5000 && dev.stats.compFailed.load() == refused; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_GT(dev.stats.compFailed.load(), refused) << "the larger ask was refused";
    const size_t freed = kernel_model::model.freed.size();
    dev.destroyTexture(opened);
    EXPECT_GT(kernel_model::model.freed.size(), freed) << "given back to the kernel";
    EXPECT_EQ(raw::DeviceHarness::compReady(dev), 0u);
    dev.destroyTexture(first);
}

// A block-linear header moved to another address names it, as fx::mipChainTic's of that address does
TEST(PrivateCompHeaders, AMovedHeaderIsTheHeaderOfItsNewAddress)
{
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::mipChainLayout(256, 128, 4, 4, c));
    for (const auto &[from, to] : {std::pair<uint64_t, uint64_t>{0xc100520000, 0xc100580000},
                                   {0x1000000200, 0xffff0003fe00},
                                   {0xc100520000 + 0x40000, 0xc1005c0000}}) {
        uint32_t moved[8], made[8];
        raw::fx::mipChainTic(c, from, 0x12345678, true, 1, 3, moved, raw::fx::kTexTwoD);
        EXPECT_EQ(raw::ticAddress(moved), from);
        raw::ticAt(moved, to);
        raw::fx::mipChainTic(c, to, 0x12345678, true, 1, 3, made, raw::fx::kTexTwoD);
        EXPECT_EQ(memcmp(moved, made, 32), 0) << std::hex << from << " to " << to;
    }
}

// RAWRT-61: a view of a compressed private texture names its compressed memory; the move before an m2v twin rewrites
// the texture's header and the view's in place on the GPU (the same indices, so a gpuResourceID handed out stays good)
// and moves the view's address with it
TEST_F(DevicePrivateComp, AViewKeepsCompressedMemoryAndFollowsTheMove)
{
    raw::Texture first, t, view;
    compressedSecond(first, t, [&](raw::Texture &x) {
        return dev.createCompressedTexture(128, 128, kBGRA8, raw::kCompKindC32, x) ||
               dev.createTexture(128, 128, kBGRA8, x, false, true);
    });
    ASSERT_TRUE(dev.createView(t, 81, view));  // BGRA8Unorm_sRGB
    EXPECT_TRUE(view.comptags) << "a raw view names the compressed memory";
    EXPECT_EQ(view.va, t.va);
    const uint32_t tic = t.tic, viewTic = view.tic;
    const std::array<uint32_t, 8> before = raw::DeviceHarness::header(dev, tic);
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(t));
    EXPECT_EQ(t.tic, tic) << "the texture's header index is kept";
    EXPECT_FALSE(view.comptags);
    EXPECT_EQ(view.va, t.va) << "the view names the plain memory";
    EXPECT_EQ(view.tic, viewTic);
    const auto launched = copies(jobs);
    const auto into = [&](uint64_t va) {
        return std::find_if(launched.begin(), launched.end(), [&](const auto &l) {
                   return l.second == va;
               }) != launched.end();
    };
    EXPECT_TRUE(into(raw::DeviceHarness::ticVa(dev, tic))) << "the texture's header rewritten by the move's job";
    for (const auto &[src, dst] : launched)
        if (dst == raw::DeviceHarness::ticVa(dev, tic)) {
            uint32_t written[8];
            memcpy(written, kernel_model::model.host(src), 32);
            uint32_t want[8];
            memcpy(want, before.data(), 32);
            raw::ticAt(want, t.va);
            EXPECT_EQ(memcmp(written, want, 32), 0) << "its own header (swizzle, format) naming the plain memory";
        }
    EXPECT_TRUE(into(raw::DeviceHarness::ticVa(dev, viewTic))) << "the view's header rewritten by the move's job";
    dev.destroyTexture(view);
    dev.destroyTexture(t);
    dev.destroyTexture(first);
}

// gpu.x1.blend_exact's 4096x2056 BGRA8 target (boot 19 and boot 21: "compressed memory 40960 KiB kind 0xdb not made"):
// its compressed memory is its own comptag lines (2176 block-linear rows of 16 KiB: 272 lines, 34 MiB), not the 40 MiB
// size class a backing would take
TEST_F(DevicePrivateComp, APrivateTextureAsksItsOwnCompTagLines)
{
    raw::Texture t;
    ASSERT_FALSE(dev.createCompressedTexture(4096, 2056, kBGRA8, raw::kCompKindC32, t)) << "none made yet: asked";
    awaitMade();
    EXPECT_EQ(raw::DeviceHarness::compReadyBytes(dev), (std::vector<uint64_t>{272ull << 17}));
}

// raw_comp.h storedZetaKind: a stored one-sample Depth32Float texture on ZF32_2CZ memory the compressed-memory thread
// made, given back with the texture
TEST_F(DevicePrivateComp, AStoredDepth32FloatTakesZF32_2CZAndGivesItBack)
{
    raw::Texture first, zeta;
    compressedSecond(
        first, zeta,
        [&](raw::Texture &t) {
            return dev.createOwnedZeta(64, 64, 252, 1, true, t);
        },
        false);
    EXPECT_TRUE(bound(raw::kCompKindZF32));
    EXPECT_NE(zeta.tic, 0u);
    const size_t freedBefore = kernel_model::model.freed.size();
    dev.destroyTexture(zeta);
    EXPECT_GT(kernel_model::model.freed.size(), freedBefore);
    dev.destroyTexture(first);
}

class DeviceClearBox : public harness::DeviceTest {
protected:
    using Load = decltype(raw::ColorAttachment::load);
    raw::Texture c0, c1;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(kW, kH, kBGRA8, c0, false, true));
    }

    raw::Pipeline *state(const char *key, uint64_t hash)
    {
        raw::RuntimeRecord r;
        r.keyString = key;
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = hash;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        raw::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One pass, c0 Clear and stored, c1 loaded as `load` and stored or not, one triangle under `scissor`: its clears
    std::vector<std::array<uint32_t, 5>> pass(Load load, bool store, bool c1Read, const uint32_t scissor[4])
    {
        raw::PassDesc p;
        p.color[0].texture = &c0;
        p.color[0].load = raw::ColorAttachment::Clear;
        p.color[0].store = true;
        p.color[1].texture = &c1;
        p.color[1].load = load;
        p.color[1].store = store;
        p.count = 2;
        raw::CommandBuffer *cb = dev.begin();
        raw::RenderEncoder *e = cb ? cb->render(p) : nullptr;
        if (!e) {
            ADD_FAILURE() << "no pass";
            return {};
        }
        e->setPipeline(c1Read ? state("samples=1 c0=80:15:0 c1=115:7:0 ds=0/0 ", 0xc1)
                              : state("samples=1 c0=80:15:0 c1=115:15:0 ds=0/0 ", 0xc0));
        e->setScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        const char *why = nullptr;
        EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        e->end();
        const auto cleared = raw::DeviceHarness::clears(*e);
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return cleared;
    }
};

using Clears = std::vector<std::array<uint32_t, 5>>;
const Clears kCardClears = {{0, 0, 0, kW, kH}, {1, 224, 128, 800, 384}};

TEST_F(DeviceClearBox, AMemorylessClearOnlyTheDrawsSeeCoversTheirTiles)
{
    ASSERT_TRUE(dev.createMemoryless(kW, kH, kRGBA16F, c1));
    const uint32_t card[4] = {250, 130, 520, 250};  // pixels 250,130 to 770,380: tiles 224,128 to 800,384
    const uint64_t narrowed = dev.stats.clearsNarrowed.load();
    EXPECT_EQ(pass(raw::ColorAttachment::Clear, false, true, card), kCardClears) << "the stored c0 keeps Metal's reach";
    EXPECT_EQ(dev.stats.clearsNarrowed.load(), narrowed + 1);
    EXPECT_EQ(pass(raw::ColorAttachment::Clear, false, false, card), (Clears{{0, 0, 0, kW, kH}}))
        << "no draw reads c1: nothing sees its clear";
    EXPECT_EQ(pass(raw::ColorAttachment::DontCare, false, true, card), kCardClears)
        << "a DontCare memoryless c1 a draw reads: zero over the draws' tiles";
    EXPECT_EQ(pass(raw::ColorAttachment::Clear, true, true, card), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}))
        << "a memoryless c1 kept for a later pass (stored) clears whole";
    EXPECT_EQ(pass(raw::ColorAttachment::Clear, true, false, card), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}));
    const uint32_t whole[4] = {0, 0, kW, kH};
    EXPECT_EQ(pass(raw::ColorAttachment::Clear, false, true, whole), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}));
}

TEST_F(DeviceClearBox, AStoredClearKeepsMetalsReach)
{
    ASSERT_TRUE(dev.createTexture(kW, kH, kRGBA16F, c1, false, true));
    const uint32_t card[4] = {250, 130, 520, 250};
    EXPECT_EQ(pass(raw::ColorAttachment::Clear, true, true, card), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}));
    EXPECT_EQ(pass(raw::ColorAttachment::Clear, false, true, card), kCardClears)
        << "a DontCare store: nothing after the pass reads it";
}

}  // namespace
