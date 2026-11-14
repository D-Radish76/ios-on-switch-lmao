// SPDX-License-Identifier: MIT
// Every compile the Metal bridge starts runs under compile_budget(), which has no deadline: the translator's work-unit cap
// and memory ledger bound it, so the same compile never fails under load and passes later.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(BridgeBudget, EveryBridgeCompileRunsWithoutADeadline)
{
    const std::string bridge = SourceText("../bridge/NativeMetalBridge.rs");
    EXPECT_THAT(Body(bridge, "pub(crate) fn compile_budget() -> metal2vulkan::Budget"),
                HasSubstr("metal2vulkan::Budget::default()"));
    for (const char *file : {"NativeMetalBridge.rs", "NativeMetalCompile.rs", "NativeMetalComputeArtifact.rs",
                             "NativeMetalDirect.rs", "NativeMetalDirectCompute.rs", "NativeMetalDirectTess.rs",
                             "NativeMetalFunctions.rs", "NativeMetalOffline.rs", "NativeMetalVariants.rs"}) {
        const std::string text = SourceText(std::string("../bridge/") + file);
        EXPECT_THAT(text, Not(HasSubstr("Budget::standard"))) << file;
        EXPECT_THAT(text, Not(HasSubstr("with_deadline"))) << file;
    }
}

}  // namespace
