// SPDX-License-Identifier: BSD-2-Clause-Patent
// What starts on every board, as the sources stand: the PMIC driver's first touch of I2C5 and the board thread's driver
// table. A board without the PMIC or MC_ERRORS feature (the Erista) must reach neither.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Between;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(SwitchBoardStart, NothingBehindTheI2c5GateRunsBeforeIt)
{
    const std::string start = Body(SourceText("../SwitchPMIC.cpp"), "extern \"C\" int switch_pmic_start(void)");
    const size_t gate = start.find("switch_board_has(SWITCH_FEATURE_PMIC)");
    ASSERT_NE(gate, std::string::npos);
    for (const char *token : {"tegra_i2c_init(", "readByte(", "publishRTC(", "moduleClockOn(", "PE_halt_restart =",
                              "switch_mmio_map(kPmcPA"}) {
        const size_t at = start.find(token);
        ASSERT_NE(at, std::string::npos) << token;
        EXPECT_GT(at, gate) << token;
    }
    EXPECT_EQ(start.substr(0, gate).find("tegra_"), std::string::npos);
}

TEST(SwitchBoardStart, TheCpuRailIsReadOnlyBehindTheGate)
{
    const std::string source = SourceText("../SwitchPMIC.cpp");
    EXPECT_EQ(Count(source, "readCpuRail("), 1u);
    EXPECT_EQ(Count(source, "tegra_i2c_init("), 1u);
    const std::string report = Body(source, "void reportClocks(const char *when)");
    EXPECT_THAT(report, HasSubstr("probeRail = switch_board_has(SWITCH_FEATURE_PMIC)"));
    EXPECT_THAT(report, HasSubstr("probeRail ? readCpuRail("));
}

TEST(SwitchBoardStart, OnlyTheWatchdogRunsOnEveryBoard)
{
    const std::string table =
        Between(SourceText("../../../diagnostics/switch/SwitchRootBoot.c"), "drivers[] = {", "\n    };");
    EXPECT_EQ(Count(table, "{0, "), 1u);
    EXPECT_THAT(table, HasSubstr("{0, switch_watchdog_publish,"));
    EXPECT_THAT(table, HasSubstr("{SWITCH_FEATURE_MC_ERRORS, tegra_mc_errors_start,"));
}

TEST(SwitchBoardStart, ABoardWithoutSensorsDoesNotWaitForThem)
{
    const std::string open =
        Body(SourceText("../../NativeSwitchWatchdog/NativeSwitchWatchdog.cpp"), "IOReturn NativeSwitchWatchdog::newUserClient(");
    const size_t gate = open.find("!switch_board_has(SWITCH_FEATURE_SENSORS)");
    const size_t wait = open.find("switch_sensors_wait_ready(");
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(wait, std::string::npos);
    EXPECT_LT(gate, wait);
    EXPECT_THAT(open.substr(gate, wait - gate), HasSubstr("thermalOffGated"));
}
