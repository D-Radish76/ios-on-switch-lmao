// SPDX-License-Identifier: MIT
// raw/runtime/raw_kernel_texture.h: how a raw kernel reaches each texture it binds. metal.float32_filtering's
// three-level R32F, RG32F and RGBA32F textures went to m2v (compute.image) because a mipmapped texture was taken for
// one sampled through an image of its own; its header is its chain's, every level in place. mediaanalysisd's Core Image
// kernel bound an RGBA8 texture whose rows are 16 bytes off 32 (boot 21: 326 command buffers on m2v): it is reached
// through its shadow image, not refused.
#include <gtest/gtest.h>

#include "../raw/runtime/raw_kernel_texture.h"

namespace {

using raw::KernelTextureForm;

struct Texture {
    unsigned tic = 7;
    uint8_t type = 0;
    bool planar = false, ycbcr = false, surfaceView = false, memoryless = false, xrPlanes = false;
    const Texture *standIn = nullptr, *sampled = nullptr;
    const void *chain = nullptr;

    bool converted() const
    {
        return planar || ycbcr;
    }
};

// gpu.ca.glass_split's compute read of a 'b3a8' surface's RGB10A8_2P_XR10 texture: m2v compiles the kernel for the format
// it is asked at, and at 550 reads one R32G32_UINT image of the word and the alpha that no header of the two planes is,
// so admission refused it (compute.image b 2). Asked at its stand-in's RGBA16Float, the kernel's view is the stand-in's.
TEST(KernelTexture, ATwoPlaneTextureIsCompiledForAsItsStandIn)
{
    const Texture standIn;
    Texture t;
    t.xrPlanes = true;
    t.standIn = &standIn;
    EXPECT_EQ(raw::kernelTextureFormat(t, 550, true), raw::kXrStandInFormat);
    t.standIn = nullptr;
    EXPECT_EQ(raw::kernelTextureFormat(t, 550, false), 550u) << "no stand-in: refused as it was";
    EXPECT_EQ(raw::kernelTextureFormat(Texture(), 80, false), 80u);
}

// A kernel sampling a BGR10_XR texture through a mip filter, anisotropy or mixed min and mag filters has no form over
// the codes (the translator's NotImplemented(Format)): one no kernel can store is read through its RGBA16Float
// stand-in, which the GM20B filters; one a kernel may store keeps its codes, which its stores write
TEST(KernelTexture, AnExtendedRangeTextureNoKernelStoresIsCompiledForAsItsStandIn)
{
    const Texture standIn;
    Texture codes;
    codes.standIn = &standIn;
    for (const uint32_t format : {552u, 553u, 554u, 555u}) {
        EXPECT_EQ(raw::kernelTextureFormat(codes, format, false), raw::kXrStandInFormat) << format;
        EXPECT_EQ(raw::kernelTextureFormat(codes, format, true), format) << format;
    }
    EXPECT_EQ(raw::kernelTextureFormat(codes, 80, false), 80u) << "a stand-in of another kind (an sRGB pair's)";
    codes.standIn = nullptr;
    EXPECT_EQ(raw::kernelTextureFormat(codes, 554, false), 554u);
}

// A state's reflection names the texture slots its kernel may store: a read-write or write-only texture binding's
// slots (an array's each), not a read-only one's nor a buffer's
TEST(KernelTexture, TheSlotsAKernelStoresAreItsWritableTextureBindings)
{
    struct Node {
        uint64_t values[8];
    };
    struct Range {
        uint32_t first, count;
    };
    struct View {
        const Range *stages;
        const Node *nodes;
        const uint32_t *edges;
    };
    const Node nodes[] = {{{2, 0, 0, 0, 0, 0, 0, 0}},   // texture 0, read-only
                          {{2, 1, 3, 0, 0, 0, 0, 0}},   // texture 3, read-write
                          {{2, 2, 5, 0, 0, 0, 0, 2}},   // textures 5 and 6, write-only
                          {{0, 1, 1, 0, 0, 0, 0, 0}}};  // buffer 1, read-write
    const uint32_t edges[] = {0, 1, 2, 3};
    const Range kernel = {0, 4};
    EXPECT_EQ(raw::kernelTexturesWritten(View{&kernel, nodes, edges}), 1u << 3 | 1u << 5 | 1u << 6);
}

TEST(KernelTexture, AMipmappedTextureIsSampledInPlace)
{
    Texture t;
    const int chain = 0;
    t.chain = &chain;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::InPlace);
}

TEST(KernelTexture, RowsTheHeaderCannotAddressGoThroughTheShadow)
{
    const Texture image;
    Texture t;
    t.sampled = &image;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Shadow);
    t.surfaceView = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Shadow);
}

TEST(KernelTexture, EveryOtherTermHasItsForm)
{
    const Texture other;
    Texture t;
    t.planar = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Converted);
    t = Texture();
    t.ycbcr = true;
    t.sampled = &other;  // a plane's pool image is the conversion's, not the kernel's
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Converted);
    t = Texture();
    t.standIn = &other;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::StandIn);
    t = Texture();
    t.tic = 0;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::NoHeader);
    t = Texture();
    t.memoryless = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::NoHeader);
    t = Texture();
    t.xrPlanes = true;
    t.standIn = &other;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::TwoPlanes);
}

// A memoryless RGB10A8_2P_XR10 texture's planes have no memory a kernel could reach (raw_xr_texture.cpp
// createXrMemoryless): a load reads header 0, a store fails its command buffer, as any memoryless texture's
TEST(KernelTexture, AMemorylessTwoPlaneTextureHasNoHeader)
{
    Texture t;
    t.xrPlanes = true;
    t.memoryless = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::NoHeader);
}

}  // namespace
