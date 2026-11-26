// The boot-arg and tick arithmetic of the timed reset (SwitchVmReset.h), as the kernel compiles them.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

extern "C" {
#include "diagnostics/switch/SwitchVmReset.h"
}

namespace {
enum switch_vm_reset_arg Parse(const std::string &line, unsigned *seconds)
{
    return switch_vm_reset_parse(line.c_str(), seconds);
}
}  // namespace

TEST(VmReset, AbsentMeansNoReset)
{
    unsigned s = 77;
    EXPECT_EQ(Parse("", &s), SWITCH_VM_RESET_ABSENT);
    EXPECT_EQ(Parse("-apfs_shared_datavolume", &s), SWITCH_VM_RESET_ABSENT);
    EXPECT_EQ(Parse("switch_stop_after_vmx=5 xswitch_stop_after_vm=5 switch_stop_after_v=5", &s), SWITCH_VM_RESET_ABSENT);
    EXPECT_EQ(switch_vm_reset_parse(nullptr, &s), SWITCH_VM_RESET_ABSENT);
    EXPECT_EQ(s, 77u);
}

TEST(VmReset, BareMeansTheDefaultFifteen)
{
    unsigned s = 0;
    EXPECT_EQ(Parse("-apfs_shared_datavolume switch_stop_after_vm", &s), SWITCH_VM_RESET_VALID);
    EXPECT_EQ(s, 15u);
}

TEST(VmReset, OneToSixtyAreAccepted)
{
    for (unsigned n = 1; n <= 60; ++n) {
        unsigned s = 0;
        ASSERT_EQ(Parse("a switch_stop_after_vm=" + std::to_string(n) + " b", &s), SWITCH_VM_RESET_VALID) << n;
        EXPECT_EQ(s, n);
    }
    unsigned s = 0;
    EXPECT_EQ(Parse("switch_stop_after_vm=015", &s), SWITCH_VM_RESET_VALID);
    EXPECT_EQ(s, 15u);
    EXPECT_EQ(Parse("switch_stop_after_vm=7\tx", &s), SWITCH_VM_RESET_VALID);
    EXPECT_EQ(s, 7u);
}

TEST(VmReset, EverythingElseIsInvalidAndLeavesTheOutputAlone)
{
    const char *bad[] = {"switch_stop_after_vm=",    "switch_stop_after_vm=0",     "switch_stop_after_vm=61",
                         "switch_stop_after_vm=-1",   "switch_stop_after_vm=+5",    "switch_stop_after_vm=5s",
                         "switch_stop_after_vm=0x10", "switch_stop_after_vm=1.5",   "switch_stop_after_vm=١",
                         "switch_stop_after_vm=99999999999999999999999999",
                         "switch_stop_after_vm=4294967296", "switch_stop_after_vm=4294967311",
                         "switch_stop_after_vm=00000000000000000000000000000000000000000000000000061",
                         "switch_stop_after_vm=5 switch_stop_after_vm=5", "switch_stop_after_vm switch_stop_after_vm=5",
                         "switch_stop_after_vm switch_stop_after_vm", "switch_stop_after_vm=5 switch_stop_after_vm=",
                         "switch_stop_after_vm=="};
    for (const char *line : bad) {
        unsigned s = 42;
        EXPECT_EQ(switch_vm_reset_parse(line, &s), SWITCH_VM_RESET_INVALID) << line;
        EXPECT_EQ(s, 42u) << line;
    }
}

TEST(VmReset, ALineWithoutATerminatorIsReadOnlyToThePermittedLength)
{
    std::string line(SWITCH_VM_RESET_LINE_BYTES + 64, 'a');
    line += " switch_stop_after_vm=5";
    unsigned s = 0;
    EXPECT_EQ(Parse(line, &s), SWITCH_VM_RESET_ABSENT);
}

TEST(VmReset, TicksAreExactAndOverflowIsRefused)
{
    EXPECT_EQ(switch_vm_reset_ticks(19200000, 15), UINT64_C(288000000));
    EXPECT_EQ(switch_vm_reset_ticks(19200000, 60), UINT64_C(1152000000));
    EXPECT_EQ(switch_vm_reset_ticks(0, 15), 0u);
    EXPECT_EQ(switch_vm_reset_ticks(19200000, 0), 0u);
    EXPECT_EQ(switch_vm_reset_ticks(19200000, 61), 0u);
    EXPECT_EQ(switch_vm_reset_ticks(UINT64_MAX, 2), 0u);
    EXPECT_EQ(switch_vm_reset_ticks(UINT64_MAX / 60, 60), (UINT64_MAX / 60) * 60);
    EXPECT_EQ(switch_vm_reset_ticks(UINT64_MAX / 60 + 1, 60), 0u);
    EXPECT_EQ(switch_vm_reset_ticks(UINT64_MAX, 1), UINT64_MAX);
}

TEST(VmReset, TheDeadlineComparisonSurvivesACounterWrap)
{
    EXPECT_TRUE(switch_vm_reset_before(100, 200));
    EXPECT_FALSE(switch_vm_reset_before(200, 200));
    EXPECT_FALSE(switch_vm_reset_before(300, 200));
    EXPECT_TRUE(switch_vm_reset_before(UINT64_MAX - 5, 10));
    EXPECT_FALSE(switch_vm_reset_before(10, UINT64_MAX - 5));
}

TEST(VmReset, ThePsciFunctionIdIsSystemReset)
{
    EXPECT_EQ(SWITCH_PSCI_SYSTEM_RESET, 0x84000009u);
}
