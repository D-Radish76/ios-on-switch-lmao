// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace native_planes {
constexpr uint32_t kOptions = 0, kWinEnable = 1u << 30;
}

namespace {
struct Display {
    static constexpr uint32_t kWinABase = 0xb80, kWinBBase = 0xd80, kWinCBase = 0xf80;
    static constexpr uint32_t kWinAUpdate = 1u << 9, kWinBUpdate = 1u << 10, kWinCUpdate = 1u << 11;
    static constexpr uint32_t kWinAActReq = 2, kWinBActReq = 4, kWinCActReq = 8;
    static constexpr uint32_t kGeneralUpdate = 1u << 8, kGeneralActReq = 1;
    static constexpr uint32_t kDcStateAccess = 0x40, kDcStateControl = 0x41;
    static constexpr uint32_t kReadMuxActive = 1, kWriteMuxActive = 4;
    static constexpr uint32_t kAll = kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq;
    std::array<uint32_t, 3> assembly{}, armed{}, active{};
    uint32_t access = 0, requested = 0;
    unsigned barriers = 0;
    std::vector<uint32_t> controlWrites;
    std::function<void(uint32_t)> beforeControlWrite;

    static unsigned window(uint32_t word)
    {
        switch (word) {
        case kWinABase:
            return 0;
        case kWinBBase:
            return 1;
        case kWinCBase:
            return 2;
        default:
            throw std::runtime_error("unexpected DC register");
        }
    }

    uint32_t read(uint32_t word) const
    {
        if (word == kDcStateAccess)
            return access;
        if (word == kDcStateControl)
            return requested;
        return (access & kReadMuxActive ? active : assembly).at(window(word));
    }

    void write(uint32_t word, uint32_t value)
    {
        if (word == kDcStateAccess) {
            access = value;
            return;
        }
        if (word != kDcStateControl) {
            (access & kWriteMuxActive ? active : assembly).at(window(word)) = value;
            return;
        }
        if (beforeControlWrite)
            beforeControlWrite(value);
        controlWrites.push_back(value);
        for (unsigned i = 0; i != 3; ++i)
            if (value & (kWinAUpdate << i))
                armed[i] = assembly[i];
        requested |= value & kAll;
    }

    struct Words {
        Display &display;

        struct Ref {
            Display &display;
            uint32_t word;

            operator uint32_t() const
            {
                return display.read(word);
            }

            void operator=(uint32_t value) const
            {
                display.write(word, value);
            }
        };

        Ref operator[](uint32_t word) const
        {
            return {display, word};
        }
    } dc{*this};

    void scanoutMemoryBarrier()
    {
        ++barriers;
    }

    void boundary(uint32_t latched = kAll)
    {
        for (unsigned i = 0; i != 3; ++i)
            if (requested & latched & (kWinAActReq << i))
                active[i] = armed[i];
        requested &= ~latched;
    }

#include "../SwitchWindowOffDisplay.h"
};

TEST(WindowOff, AssemblyWritesDoNotProveTheActiveFrontWasReleased)
{
    Display display;
    display.assembly.fill(native_planes::kWinEnable);
    display.active = display.armed = display.assembly;
    display.requestWindowsOff(Display::kAll);
    EXPECT_EQ(display.assembly, (std::array<uint32_t, 3>{}));
    EXPECT_FALSE(display.windowsOffTaken(Display::kAll));
    EXPECT_EQ(display.active[0], native_planes::kWinEnable);
    display.boundary();
    EXPECT_TRUE(display.windowsOffTaken(Display::kAll));
    EXPECT_EQ(display.barriers, 1u);
    EXPECT_EQ(display.controlWrites.size(), 2u);
}

TEST(WindowOff, LandscapeUiAndBothOverlaysMustAllBeInactive)
{
    for (unsigned live = 0; live != 3; ++live) {
        Display display;
        display.active[live] = native_planes::kWinEnable;
        display.requestWindowsOff(Display::kAll);
        display.requested = 0;
        EXPECT_FALSE(display.windowsOffTaken(Display::kAll)) << "live window " << live;
        display.active[live] = 0;
        EXPECT_TRUE(display.windowsOffTaken(Display::kAll));
    }
}

TEST(WindowOff, AnOutstandingRequestCannotBeMistakenForAnOffLatch)
{
    for (const uint32_t request : {Display::kGeneralActReq, Display::kWinAActReq, Display::kWinBActReq,
                                   Display::kWinCActReq}) {
        Display display;
        display.requested = request;
        EXPECT_FALSE(display.windowsOffTaken(Display::kAll));
        display.boundary();
        EXPECT_TRUE(display.windowsOffTaken(Display::kAll));
    }
}

TEST(WindowOff, AFrameEndBeforeTheOffActRequestDoesNotCompleteIt)
{
    Display display;
    display.active.fill(native_planes::kWinEnable);
    display.armed = display.active;
    display.beforeControlWrite = [&display](uint32_t value) {
        if (value == Display::kAll)
            display.boundary();
    };
    display.requestWindowsOff(Display::kAll);
    EXPECT_FALSE(display.windowsOffTaken(Display::kAll));
    EXPECT_EQ(display.active[1], native_planes::kWinEnable);
    display.boundary();
    EXPECT_TRUE(display.windowsOffTaken(Display::kAll));
}

TEST(WindowOff, OverlayRecoveryLeavesWindowAAlone)
{
    Display display;
    display.active.fill(native_planes::kWinEnable);
    display.assembly = display.armed = display.active;
    constexpr uint32_t overlays = Display::kWinBActReq | Display::kWinCActReq;
    display.requestWindowsOff(overlays);
    EXPECT_EQ(display.assembly[0], native_planes::kWinEnable);
    display.boundary();
    EXPECT_TRUE(display.windowsOffTaken(overlays));
    EXPECT_FALSE(display.windowsOffTaken(Display::kAll));
    EXPECT_EQ(display.active[0], native_planes::kWinEnable);
}

TEST(WindowOff, RecheckingAMissingLatchNeverReissuesTheOffRequest)
{
    Display display;
    display.active.fill(native_planes::kWinEnable);
    display.requestWindowsOff(Display::kAll);
    for (unsigned frame = 0; frame != 4; ++frame)
        EXPECT_FALSE(display.windowsOffTaken(Display::kAll));
    EXPECT_EQ(display.controlWrites.size(), 2u);
    EXPECT_EQ(display.barriers, 1u);
}

TEST(WindowOff, ActiveReadsReturnToTheAssemblyBankWithTheWriteMuxOff)
{
    Display display;
    display.access = Display::kReadMuxActive | Display::kWriteMuxActive | 0x20;
    EXPECT_TRUE(display.windowsOffTaken(Display::kAll));
    EXPECT_EQ(display.access, 0x20u);
}
}  // namespace
