// SPDX-License-Identifier: MIT
// raw/runtime/raw_device_open.h: a process's Metal device that did not open fails that ask alone, its class named, and
// the next ask opens afresh; the raw device carries the cause out (RawMetal.mm) and a prepared raw device that did not open
// is prepared again.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../raw/runtime/raw_device_open.h"
#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(RawDeviceOpen, TheRawDeviceCarriesTheClassOfWhatItMetToThePluginsLine)
{
    const std::string facade = SourceText("../raw/runtime/RawMetal.mm");
    EXPECT_THAT(facade, HasSubstr("failure = dev->openFailure();"));
    EXPECT_THAT(Body(facade, "id<MTLDevice> raw_metal_device_open("), HasSubstr("gOpenFailure = failure;"));
    const std::string gpu = SourceText("../raw/runtime/raw_gpu.cpp");
    const std::string open =
        Body(gpu, "bool Device::open(const char *packPath, uint64_t stack, std::string &err, Role role)");
    EXPECT_LT(open.find("openFailure_ = OpenFailure::Structural;"), open.find("pack_.load("));
    EXPECT_LT(open.find("openFailure_ = OpenFailure::Transient;"), open.find("k_.open(err)"));
    EXPECT_THAT(Body(SourceText("../TegraGM20BMetalPlugin.mm"),
                     "id openOnce(unsigned port, raw::OpenFailure &failure, NSString *__strong &why)"),
                HasSubstr("failure = raw_metal_open_failure();"));
}

// A device that did not open fails that ask alone (nil, one error line naming its class); no count, no latch: the next
// ask opens afresh
TEST(RawDeviceOpen, EveryAskOfAProcessWithNoDeviceOpensAfresh)
{
    const std::string plugin = SourceText("../TegraGM20BMetalPlugin.mm");
    EXPECT_THAT(plugin, Not(HasSubstr("static bool tried")));
    EXPECT_THAT(plugin, Not(HasSubstr("DeviceOpens")));
    const std::string open = Body(plugin, "id openDevice(unsigned port)");
    EXPECT_THAT(open, HasSubstr("if (device)\n            return device;"));
    EXPECT_THAT(open, HasSubstr("device = openOnce(port, failure, why);"));
    EXPECT_THAT(open, HasSubstr("raw::openFailureText(failure), why);"));
}

TEST(RawDeviceOpen, APreparedRawDeviceThatDidNotOpenIsPreparedAgain)
{
    const std::string facade = SourceText("../raw/runtime/RawMetal.mm");
    const std::string open = Body(facade, "id<MTLDevice> raw_metal_device_open(");
    EXPECT_THAT(open, HasSubstr("gPrepared.started = dev != nullptr;"));
    EXPECT_THAT(open, HasSubstr("gOpenFailure = failure;"));
}

}  // namespace
