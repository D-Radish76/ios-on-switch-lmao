"""The stop paths of ERISTA_STOP_PATHS.md as the sources stand."""
import re
import unittest
from pathlib import Path


def text(path):
    return Path(path).read_text()


HALT = text("kernel/xnu-10063.141.1/osfmk/arm64/machine_routines.c")
IOPE = text("kernel/xnu-10063.141.1/iokit/Kernel/IOPlatformExpert.cpp")
ARMCPU = text("drivers/ArmCpu/ArmCpu.cpp")
PMIC = text("drivers/SwitchPower/SwitchPMIC.cpp")
SAVE = text("diagnostics/switch/SwitchPanicSave.c")
ERISTA_ADT = text("tools/package/native_erista_adt.py")
CAUSE = text("diagnostics/switch/SwitchResetCause.h")


class Gaps(unittest.TestCase):
    def test_a_normal_stop_without_a_halt_hook_spins(self):
        body = HALT[HALT.index("halt_all_cpus(boolean_t reboot)") :]
        body = body[: body.index("\n}\n")]
        self.assertRegex(body, r"PEHaltRestart\(kPERestartCPU\);")
        self.assertRegex(body, r"PEHaltRestart\(kPEHaltCPU\);\s*\}\s*while \(1\)")
        self.assertNotIn("reset", body.lower())

    def test_the_default_hook_is_null_and_the_platform_expert_returns_minus_one_without_it(self):
        self.assertIn("int (*PE_halt_restart)(unsigned int type) = NULL;", IOPE)
        self.assertRegex(IOPE, r"if \(PE_halt_restart\) \{\s*return \(\*PE_halt_restart\)\(type\);\s*\} else \{\s*return -1;")

    def test_only_the_pmic_driver_installs_the_hook(self):
        owners = [
            str(p)
            for root in ("drivers", "diagnostics")
            for p in Path(root).rglob("*")
            if p.suffix in (".c", ".cpp", ".mm") and "test" not in p.parts and re.search(r"PE_halt_restart\s*=", p.read_text())
        ]
        self.assertEqual(owners, ["drivers/SwitchPower/SwitchPMIC.cpp"])

    def test_the_pmic_driver_installs_it_only_with_the_feature(self):
        start = PMIC[PMIC.index("switch_pmic_start") :]
        self.assertRegex(start, r"if \(!pmic\)")
        self.assertIn("no halt or restart hook", PMIC)


class Psci(unittest.TestCase):
    def test_the_conduit_needs_the_node_and_on_a_gicv2_board_native_smp(self):
        body = ARMCPU[ARMCPU.index("static bool psciConduit") :]
        body = body[: body.index("\n}\n")]
        self.assertIn('fromPath("/psci"', body)
        self.assertRegex(body, r"armcpu_gic_version == 2")
        self.assertIn("&& gNativeSmp", body)

    def test_the_erista_tree_has_no_psci_node(self):
        self.assertIn("single core", ERISTA_ADT)
        self.assertNotRegex(ERISTA_ADT, r"add_child\([^)]*['\"]psci['\"]")

    def test_the_panic_reset_is_the_pmc_and_not_psci(self):
        body = SAVE[SAVE.index("static void __attribute__((noreturn)) pmc_reset") :]
        body = body[: body.index("\n}\n")]
        self.assertIn("PMC_CNTRL", body)
        self.assertNotRegex(SAVE, r"\bsmc\s*#|0x84000009")


class Resets(unittest.TestCase):
    def test_the_watchdog_reset_is_armed_from_the_panic_path_and_the_power_trail_only(self):
        callers = [
            str(p)
            for p in Path("diagnostics/switch").glob("*.c")
            if "switch_panic_save_watchdog(" in p.read_text() and p.name != "SwitchPanicSave.c"
        ]
        self.assertEqual(callers, ["diagnostics/switch/SwitchPowerTrail.c"])
        for line in SAVE.splitlines():
            if re.search(r"^\s+arm_watchdog\(", line):
                self.assertRegex(line, r"reboot_s|\(seconds\)")

    def test_a_reset_cause_names_panic_hang_and_thermal_apart(self):
        for name in ("SWITCH_RESET_WATCHDOG", "SWITCH_RESET_THERMTRIP", "SWITCH_RESET_SOFTWARE", "SWITCH_RESET_POWER_ON"):
            self.assertIn(name, CAUSE)
        self.assertRegex(CAUSE, r"if \(panic_saved\) \{\s*return 0;")


if __name__ == "__main__":
    unittest.main()
