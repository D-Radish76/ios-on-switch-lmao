// SPDX-License-Identifier: MIT
// raw/runtime/raw_draw_check.h: a draw's check runs again after every bind that can change its answer (a planar or XR
// texture bound or replaced, a texture under a cubic sampler or of a state that writes textures, a cubic sampler bound or
// replaced, a buffer, a pipeline, a depth/stencil state, an opaque call) and after no other.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_draw_check.h"

namespace {

using raw::CheckedTexture;

constexpr CheckedTexture kPlain{}, kPlanar{true, false}, kXr{false, true};

TEST(DrawCheck, APlainTextureOverAPlainOneLeavesTheAnswer)
{
    EXPECT_FALSE(raw::textureBindChecks(kPlain, kPlain, false, false));
}

TEST(DrawCheck, APlanarOrXrTextureBoundOrReplacedChecksAgain)
{
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kPlanar, false, false)) << "a constexpr sampler refuses it";
    EXPECT_TRUE(raw::textureBindChecks(kPlanar, kPlain, false, false)) << "the refusal it made goes";
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kXr, false, false)) << "its planes take a variant";
    EXPECT_TRUE(raw::textureBindChecks(kXr, kPlain, false, false));
}

TEST(DrawCheck, ATextureUnderACubicSamplerOrOfAStateThatWritesTexturesChecksAgain)
{
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kPlain, true, false));
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kPlain, false, true));
}

}  // namespace
