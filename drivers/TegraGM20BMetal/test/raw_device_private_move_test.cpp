// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.cpp on raw::Device over the kernel model: a private target made plain because no
// compressed memory of its size was ready at its creation moves onto that memory once it is made (Device::wantCompressed,
// privateMoves, compressedFromPlain), as the submit worker moves it at its next raw range, and a look is due only where
// memory that serves a want came.
#include "raw_device_private_comp.h"
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using private_comp::copies;
using private_comp::DevicePrivateComp;
using private_comp::kBGRA8;
using private_comp::kH;
using private_comp::kW;

std::vector<void *> taken;

void take(void *owner)
{
    taken.push_back(owner);
}

// The owners the worker's look takes
std::vector<void *> look(raw::Device &d)
{
    taken.clear();
    d.privateMoves(take);
    return taken;
}

// Core Animation's zoom window target made plain for want of compressed memory: once some is made, the worker moves it,
// its texels by the copy engine in a job of its own that then rewrites its header and the one viewTic made from it in
// place
TEST_F(DevicePrivateComp, APlainTargetMovesOnceItsMemoryIsMade)
{
    raw::Texture t, other;
    zoomTarget(t);
    ASSERT_FALSE(t.comptags) << "none ready at its creation: asked for";
    awaitMade();
    EXPECT_TRUE(dev.privateMoveDue()) << "memory made that serves a want";
    ASSERT_TRUE(dev.createTexture(64, 64, kBGRA8, other, false, true));
    EXPECT_EQ(look(dev), std::vector<void *>{&t});
    EXPECT_FALSE(dev.privateMoveDue()) << "the look clears it";
    const uint32_t derived = dev.viewTic(t, raw::formatInfo(81)->tic0);  // BGRA8Unorm_sRGB
    ASSERT_NE(derived, 0u);
    commitEmpty();
    const uint64_t plainVa = t.va;
    const uint32_t tic = t.tic;
    const std::array<uint32_t, 8> own = raw::DeviceHarness::header(dev, tic),
                                  view = raw::DeviceHarness::header(dev, derived);
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.compressedFromPlain(t));
    EXPECT_TRUE(t.comptags);
    EXPECT_NE(t.va, plainVa);
    EXPECT_EQ(t.tic, tic) << "a gpuResourceID handed out names the same header";
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs + 1) << "the move is a job of its own";
    const auto launched = copies(jobs);
    ASSERT_FALSE(launched.empty());
    EXPECT_EQ(launched.front(), (std::pair<uint64_t, uint64_t>{plainVa, t.va})) << "its texels first";
    for (const auto &[header, before] : {std::make_pair(tic, own), std::make_pair(derived, view)}) {
        uint32_t want[8];
        memcpy(want, before.data(), 32);
        raw::ticAt(want, t.va);
        const uint64_t headerVa = raw::DeviceHarness::ticVa(dev, header);
        const auto into = std::find_if(launched.begin(), launched.end(), [&](const auto &l) {
            return l.second == headerVa;
        });
        ASSERT_NE(into, launched.end()) << "header " << header << " rewritten by the move's job";
        EXPECT_EQ(memcmp(kernel_model::model.host(into->first), want, 32), 0) << "header " << header;
    }
    EXPECT_EQ(dev.stats.privateCompressedMoves.load(), 1u);
    EXPECT_TRUE(dev.privateCompressed(t));
    EXPECT_EQ(raw::DeviceHarness::wanting(dev), 0u);
    EXPECT_FALSE(dev.compressedFromPlain(t)) << "once";
    EXPECT_EQ(raw::DeviceHarness::movedPlain(dev), 1u);
    commitEmpty();
    EXPECT_EQ(raw::DeviceHarness::movedPlain(dev), 0u) << "the plain memory went once the move's job completed";
    dev.destroyTexture(other);
    dev.destroyTexture(t);
}

// Every maker of a header over a target's memory, around a move. One made before it keeps its index, and the move's job
// rewrites it: the same key asked again answers that index (BGRA8Unorm and BGRA8Unorm_sRGB share word 0, sRGB being a
// header bit, so their views ask one key). One made after it, before that job ran, names the compressed memory. The
// model runs no job, so its table keeps a header the job rewrites as it was; the job's launches show the rewrite.
TEST_F(DevicePrivateComp, EveryHeaderOverAMovedTargetNamesItsNewMemory)
{
    raw::Texture t, view, part;
    zoomTarget(t);
    awaitMade();
    const uint32_t bgra8 = raw::formatInfo(kBGRA8)->tic0;
    raw::AttachmentView own;
    own.word0 = bgra8;
    own.layers = 1;
    const char *why = nullptr;
    const uint32_t before[] = {dev.viewTic(t, bgra8), dev.attachmentViewTic(t, own, &why),
                               dev.attachmentTic(t, t, 0, 0)};
    std::vector<std::array<uint32_t, 8>> words;
    for (const uint32_t tic : before) {
        ASSERT_NE(tic, 0u) << (why ? why : "");
        words.push_back(raw::DeviceHarness::header(dev, tic));
    }
    commitEmpty();
    const uint64_t plainVa = t.va;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.compressedFromPlain(t));
    const auto launched = copies(jobs);
    for (size_t i = 0; i < words.size(); i++) {
        EXPECT_EQ(raw::ticAddress(words[i].data()), plainVa) << "maker " << i;
        uint32_t want[8];
        memcpy(want, words[i].data(), 32);
        raw::ticAt(want, t.va);
        const uint64_t headerVa = raw::DeviceHarness::ticVa(dev, before[i]);
        const auto into = std::find_if(launched.begin(), launched.end(), [&](const auto &l) {
            return l.second == headerVa;
        });
        ASSERT_NE(into, launched.end()) << "maker " << i << ": rewritten by the move's job";
        EXPECT_EQ(memcmp(kernel_model::model.host(into->first), want, 32), 0) << "maker " << i;
    }
    EXPECT_EQ(dev.viewTic(t, raw::formatInfo(81)->tic0), before[0]);
    EXPECT_EQ(dev.attachmentViewTic(t, own, &why), before[1]);
    EXPECT_EQ(dev.attachmentTic(t, t, 0, 0), before[2]);
    raw::AttachmentView other = own;
    other.word0 = raw::formatInfo(53)->tic0;  // R32Uint
    raw::Texture srgb = t;
    srgb.format = 81;
    ASSERT_TRUE(dev.createView(t, 81, view));
    ASSERT_TRUE(dev.createPartView(t, kBGRA8, raw::fx::kTexTwoD, 0, 1, 0, 1, part));
    const std::pair<const char *, uint32_t> after[] = {
        {"viewTic", dev.viewTic(t, raw::formatInfo(70)->tic0)},  // RGBA8Unorm
        {"attachmentViewTic", dev.attachmentViewTic(t, other, &why)},
        {"attachmentTic", dev.attachmentTic(t, srgb, 0, 0)},
        {"createView", view.tic},
        {"createPartView", part.tic},
    };
    for (const auto &[maker, tic] : after) {
        ASSERT_NE(tic, 0u) << maker;
        EXPECT_EQ(raw::ticAddress(raw::DeviceHarness::header(dev, tic).data()), t.va)
            << maker << ": made after the move, before its job ran";
    }
    EXPECT_TRUE(view.comptags);
    EXPECT_EQ(part.va, t.va);
    commitEmpty();
    dev.destroyTexture(part);
    dev.destroyTexture(view);
    dev.destroyTexture(t);
}

// A look clears the flag whatever it found; no count of command buffers or time sets it again, memory kept that serves
// the want does
TEST_F(DevicePrivateComp, ALookIsDueOnlyAfterMemoryThatServesAWantComes)
{
    EXPECT_FALSE(dev.privateMoveDue()) << "nothing wants compressed memory";
    raw::Texture t, kept;
    zoomTarget(t);
    awaitMade();
    ASSERT_TRUE(dev.privateMoveDue());
    EXPECT_EQ(look(dev).size(), 1u);
    for (int i = 0; i < 64; i++)
        commitEmpty();
    EXPECT_FALSE(dev.privateMoveDue());
    ASSERT_TRUE(dev.createCompressedTexture(kW, kH, kBGRA8, raw::kCompKindC32, kept)) << "the memory made";
    EXPECT_FALSE(dev.privateMoveDue());
    dev.destroyTexture(kept);
    EXPECT_TRUE(dev.privateMoveDue()) << "a closed target's memory kept that serves the want";
    dev.destroyTexture(t);
}

// A want that no ready memory serves (another texture took its memory; what comes next is of another kind) gets no look:
// nothing runs on the worker's path for it until memory that serves it comes
TEST_F(DevicePrivateComp, MemoryThatServesNoWantBringsNoLook)
{
    raw::Texture t, taker, other;
    zoomTarget(t);
    awaitMade();
    EXPECT_EQ(look(dev).size(), 1u);
    ASSERT_TRUE(dev.createCompressedTexture(kW, kH, kBGRA8, raw::kCompKindC32, taker)) << "its memory taken";
    ASSERT_FALSE(dev.createCompressedTexture(256, 256, 115, raw::kCompKindC64, other)) << "RGBA16Float: asked";
    awaitMade();
    EXPECT_FALSE(dev.privateMoveDue()) << "C64 memory serves no C32 want";
    EXPECT_TRUE(look(dev).empty());
    dev.destroyTexture(taker);
    EXPECT_TRUE(dev.privateMoveDue()) << "its kind and size kept";
    dev.destroyTexture(t);
}

// A raw view's header names the plain memory, and the view may go, its header index another texture's, before a move's
// job runs: a target with one stays plain for good
TEST_F(DevicePrivateComp, APlainTargetWithARawViewStaysPlain)
{
    raw::Texture t, view;
    zoomTarget(t);
    ASSERT_TRUE(dev.createView(t, 81, view));
    awaitMade();
    EXPECT_TRUE(look(dev).empty());
    EXPECT_EQ(raw::DeviceHarness::wanting(dev), 0u);
    const size_t jobs = kernel_model::model.jobs.size();
    EXPECT_FALSE(dev.compressedFromPlain(t));
    EXPECT_FALSE(t.comptags);
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
    dev.destroyTexture(view);
    dev.destroyTexture(t);
}

}  // namespace
