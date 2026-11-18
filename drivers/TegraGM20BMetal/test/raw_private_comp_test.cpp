// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.h: which private textures bind compressed memory, and of which kind.
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_storage_form.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

raw::PrivateTextureFacts target(uint32_t bpp)
{
    raw::PrivateTextureFacts f;
    f.privateStorage = f.compressibleUsage = f.plainColour = true;
    f.shape = raw::PrivateShape2D::Plain;
    f.compression = true;
    f.bpp = bpp;
    return f;
}

TEST(RawPrivateComp, TargetsTakeTheColourKindsTheCopyEngineReads)
{
    EXPECT_EQ(raw::privateCompressedKind(target(4)), uint32_t(raw::kCompKindC32));
    EXPECT_EQ(raw::privateCompressedKind(target(8)), uint32_t(raw::kCompKindC64));
    EXPECT_EQ(raw::privateCompressedKind(target(16)), uint32_t(raw::kCompKindC128)) << "RGBA32Float";
    EXPECT_EQ(raw::privateCompressedKind(target(2)), 0u);
}

TEST(RawPrivateComp, AnythingElseStaysPlain)
{
    bool raw::PrivateTextureFacts::*fields[] = {
        &raw::PrivateTextureFacts::privateStorage,
        &raw::PrivateTextureFacts::compressibleUsage,
        &raw::PrivateTextureFacts::plainColour,
        &raw::PrivateTextureFacts::compression,
    };
    for (auto field : fields) {
        raw::PrivateTextureFacts f = target(4);
        f.*field = false;
        EXPECT_EQ(raw::privateCompressedKind(f), 0u);
    }
    raw::PrivateTextureFacts other = target(4);
    other.shape = raw::PrivateShape2D::Other;
    EXPECT_EQ(raw::privateCompressedKind(other), 0u) << "a cube, a 3D or a 1D texture";
}

TEST(RawPrivateComp, ShaderWriteUsageIsTakenWithStorageReadingCompressed)
{
    const std::string facade =
        Body(SourceText("../raw/runtime/RawMetalPrivateComp.mm"), "uint32_t privateCompressedKind(");
    EXPECT_THAT(facade,
                HasSubstr("MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite"))
        << "gpu.raw.private_compression shader_write";
    EXPECT_TRUE(raw::storageReadsCompressed());
}

TEST(RawPrivateComp, AResourceIdLeavesACompressedTextureOnItsMemory)
{
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawMetalResourceIds.mm"), "- (MTLResourceID)gpuResourceID"),
                Not(HasSubstr("rmTexturePlain")));
}

TEST(RawPrivateComp, AMipChainTakesItsKind)
{
    raw::PrivateTextureFacts f = target(4);
    f.levels = 5;
    EXPECT_EQ(raw::privateCompressedKind(f), uint32_t(raw::kCompKindC32)) << "gpu.raw.private_compression mips";
}

TEST(RawPrivateComp, AOneLevelArrayTakesItsKind)
{
    raw::PrivateTextureFacts f = target(8);
    f.shape = raw::PrivateShape2D::Array;
    EXPECT_EQ(raw::privateCompressedKind(f), uint32_t(raw::kCompKindC64)) << "gpu.raw.private_compression array";
    f.levels = 2;
    EXPECT_EQ(raw::privateCompressedKind(f), 0u) << "an array's levels are no chain the move copies";
}

// A stored multisampled target takes its sample count's kind where the copy engine reads that kind (the move to plain
// memory is its copy of the sample grid)
TEST(RawPrivateComp, AStoredMultisampledTargetTakesItsKindWhereTheCopyEngineReadsIt)
{
    raw::PrivateTextureFacts f = target(4);
    f.shape = raw::PrivateShape2D::Multisample;
    f.samples = 4;
    const uint32_t kind = raw::compressedKind(4, 0, 4);
    EXPECT_EQ(raw::privateCompressedKind(f), raw::copyEngineReadsCompressed(kind) ? kind : 0u);
    EXPECT_TRUE(raw::privateShapeCompresses(f)) << "gpu.raw.private_compression ms4";
    f.samples = 8;
    EXPECT_FALSE(raw::privateShapeCompresses(f));
}

// The copy engine moves a multisampled texture as fx::msLayout's grid of its samples
TEST(RawPrivateComp, AMultisampledTextureMovesAsItsSampleGrid)
{
    uint32_t sx = 9, sy = 9;
    raw::sampleGrid(1, sx, sy);
    EXPECT_EQ(sx, 0u);
    EXPECT_EQ(sy, 0u);
    raw::sampleGrid(2, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 0u);
    raw::sampleGrid(4, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 1u);
}

TEST(RawPrivateComp, EveryLaidOutShapeMovesWholeAndKeepsWhatWorkSubmittedReads)
{
    const std::string source = SourceText("../raw/runtime/raw_private_comp.cpp");
    const std::string move = Body(source, "bool Device::plainFromCompressed(");
    EXPECT_THAT(move, HasSubstr("createLaidOut(ask, plain, nullptr)")) << "a chain, array or multisampled texture";
    EXPECT_THAT(move, HasSubstr("copyImages(*b, t, plain);"));
    EXPECT_THAT(move, HasSubstr("p.staleChain = t.chain;"));
    const std::string copy = Body(source, "void copyImages(");
    EXPECT_THAT(copy, HasSubstr("b.copyTexels(from, 0, 0, 0, to, 0, 0, 0, from.width, from.height);")) << "level 0";
    EXPECT_THAT(copy, HasSubstr("chainLevelImage(from, level, a)")) << "every later level";
    EXPECT_THAT(copy, HasSubstr("layer < std::max(from.layers, 1u)")) << "every layer";
    EXPECT_THAT(copy, HasSubstr("sampleGrid(from.samples, sx, sy)")) << "every sample";
    EXPECT_THAT(Body(source, "void Device::releasePrivateCompressed("), HasSubstr("delete stale;"));
    const std::string laid = Body(SourceText("../raw/runtime/raw_textures.cpp"), "bool Device::createLaidOut(");
    EXPECT_THAT(laid, HasSubstr("ask.compressKind && !at && !zeta && takeCompressed(bytes, ask.compressKind, comp)"));
    EXPECT_THAT(laid, HasSubstr("noteCompressed(ask, comp, bytes, t);"));
}

TEST(RawPrivateComp, AStoredZetaTakesCompressedMemoryInEveryBuild)
{
    const std::string zeta = Body(SourceText("../raw/runtime/raw_multisample.cpp"), "bool Device::createOwnedZeta(");
    EXPECT_THAT(zeta, HasSubstr("storedZetaKind("));
    EXPECT_THAT(zeta, HasSubstr("createCompressedZeta(bytes, kind, t)"));
    EXPECT_THAT(zeta, Not(HasSubstr("#if"))) << "the compressed route is the release one, not a probe's";
    const std::string made =
        Body(SourceText("../raw/runtime/raw_private_comp.cpp"), "bool Device::createCompressedZeta(");
    EXPECT_THAT(made, HasSubstr("takeCompressed(bytes, kind, bo)")) << "memory made off the render thread, as colour's";
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_private_comp.cpp"), "bool Device::takeCompressed("),
                HasSubstr("comp_.fit(need, kind)"));
    EXPECT_THAT(made, Not(HasSubstr("allocCompressible")));
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_gpu.h"), "    void freeOwnedZeta(uint64_t va)"),
                HasSubstr("freeCompressedZeta(va);"));
}

}  // namespace
