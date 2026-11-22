// SPDX-License-Identifier: MIT
// Every descriptor shape that Metal, an identity and GM20B all make has a raw creator (raw_texture_shape.h route), so
// newTextureWithDescriptor: never finds a shape with none: every format Metal describes, of every type and storage mode,
// at the level, sample and layer counts that tell the creators apart.
#include "../raw/runtime/raw_decoded.h"
#include "../raw/runtime/raw_gpu.h"
#include "../raw/runtime/raw_metal_formats.h"
#include "../raw/runtime/raw_texture_shape.h"
#include <gtest/gtest.h>
#include <sstream>
#include <vector>

namespace {

using raw::shapes::TextureShape;

std::string describe(const TextureShape &s)
{
    std::ostringstream o;
    o << "format " << s.format << " type " << s.type << " storage " << s.storage << " levels " << s.levels
      << " samples " << s.samples << " array " << s.array;
    return o.str();
}

TEST(TextureRoutes, EveryShapeMadeHasACreator)
{
    std::vector<std::string> unrouted;
    uint32_t made = 0;
    for (const raw::answers::Kind &kind : raw::answers::kKinds)
        for (const raw::metal_formats::Described &f : raw::metal_formats::kDescribed)
            for (uint32_t type = 0; type <= 9; type++)
                for (uint32_t storage : {raw::shapes::kShared, raw::shapes::kManaged, raw::shapes::kPrivate,
                                         raw::shapes::kMemoryless})
                    for (uint64_t levels : {1u, 2u})
                        for (uint64_t samples : {1u, 2u, 4u})
                            for (uint64_t array : {1u, 2u})
                                for (uint64_t usage : {uint64_t(0), uint64_t(raw::shapes::kUsageRead),
                                                       uint64_t(raw::shapes::kUsageRead | raw::shapes::kUsageTarget)}) {
                                    TextureShape s;
                                    s.format = f.mtl;
                                    s.type = type;
                                    s.storage = storage;
                                    s.levels = levels;
                                    s.samples = samples;
                                    s.array = array;
                                    s.usage = usage;
                                    s.width = 16;
                                    s.height = type == raw::kType1D || type == raw::kType1DArray || type == 9 ? 1 : 16;
                                    s.depth = type == raw::kType3D ? 4 : 1;
                                    s.ownMemory = true;
                                    // a texture buffer of a header row is made over a buffer of the device's
                                    // (RawMetalDevice.mm), X32_Stencil8 as its combined format's view
                                    if (raw::shapes::metalRefusal(s) || raw::shapes::identityRefusal(s, kind) ||
                                        raw::shapes::hardwareRefusal(s) || (type == 9 && raw::formatInfo(f.mtl)) ||
                                        f.mtl == 261)
                                        continue;
                                    made++;
                                    if (raw::shapes::route(s) == raw::shapes::Route::None && unrouted.size() < 16)
                                        unrouted.push_back(std::string(kind.kind) + " " + describe(s));
                                }
    printf("  %u shapes made, %zu with no creator\n", made, unrouted.size());
    EXPECT_GT(made, 0u);
    EXPECT_EQ(unrouted, std::vector<std::string>{});
}

// A one-level 3D render target of a format the GM20B renders none of is laid out one plane a block, as one it renders is:
// a pass into a plane renders its stand-in from and into that plane alone, with no copy of a block's planes around it
TEST(TextureRoutes, AVolumeTargetOfAStandInFormatIsLaidOutOnePlaneABlock)
{
    for (const uint32_t format : {11u /* R8Unorm_sRGB */, 41u /* A1BGR5Unorm */, 553u /* BGRA10_XR_sRGB */}) {
        TextureShape s;
        s.type = raw::kType3D;
        s.format = format;
        s.storage = raw::shapes::kPrivate;
        s.usage = raw::shapes::kUsageRead | raw::shapes::kUsageTarget;
        s.width = s.height = 32;
        s.depth = 4;
        EXPECT_TRUE(raw::shapes::volumeView2d(s)) << format;
        s.usage = raw::shapes::kUsageRead;
        EXPECT_FALSE(raw::shapes::volumeView2d(s)) << format << ": sampled alone";
    }
}

// A texture of a format decoded on upload (PVRTC, ASTC 4x2 and 8x4, ASTC HDR) is made as the private image of its stored
// format, laid out as a render target (its decode passes draw into it), whatever the storage mode asked: GM20B refuses none
TEST(TextureRoutes, ADecodedFormatsTextureIsItsStoredFormatsImage)
{
    for (const raw::DecodedFormat &f : raw::kDecodedFormats)
        for (uint32_t type : {raw::kType2D, raw::kType2DArray, raw::kTypeCube, raw::kType3D})
            for (uint32_t storage : {raw::shapes::kShared, raw::shapes::kPrivate})
                for (uint64_t levels : {1u, 3u}) {
                    TextureShape s;
                    s.format = f.format;
                    s.type = type;
                    s.storage = storage;
                    s.levels = levels;
                    s.width = s.height = 16;
                    s.depth = type == raw::kType3D ? 4 : 1;
                    s.array = type == raw::kType2DArray ? 3 : 1;
                    s.usage = raw::shapes::kUsageRead;
                    s.ownMemory = true;
                    if (raw::shapes::metalRefusal(s))
                        continue;
                    EXPECT_EQ(raw::shapes::hardwareRefusal(s), nullptr) << describe(s);
                    const TextureShape held = raw::shapes::heldShape(s);
                    EXPECT_EQ(held.format, f.stored) << describe(s);
                    EXPECT_EQ(held.storage, raw::shapes::kPrivate) << describe(s);
                    EXPECT_NE(held.usage & raw::shapes::kUsageTarget, 0u) << describe(s);
                    EXPECT_NE(raw::shapes::route(s), raw::shapes::Route::None) << describe(s);
                    EXPECT_EQ(raw::shapes::route(s), raw::shapes::route(held)) << describe(s);
                }
}

// Metal makes no texture buffer of a block-compressed format: an M4 Max's validation stops BC1, PVRTC and ASTC alike
TEST(TextureRoutes, ABlockCompressedFormatTakesNoTextureBuffer)
{
    for (const uint32_t format : {130u, 166u, 203u, 204u, 222u}) {
        TextureShape s;
        s.format = format;
        s.type = 9;
        s.width = 64;
        EXPECT_NE(raw::shapes::metalRefusal(s), nullptr) << format;
    }
}

// MTLPixelFormatUnspecialized names no texture's format (an M4 Max; both identities answer it as Invalid)
TEST(TextureRoutes, UnspecializedIsNoTexturesFormat)
{
    TextureShape s;
    s.format = 263;
    EXPECT_NE(raw::shapes::metalRefusal(s), nullptr);
    EXPECT_NE(raw::shapes::formatRefusal(263), nullptr);
    EXPECT_EQ(raw::shapes::formatRefusal(53), nullptr) << "R32Uint";
}

}  // namespace
