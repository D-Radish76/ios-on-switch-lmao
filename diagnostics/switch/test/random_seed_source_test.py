"""The facts ERISTA_RANDOM_SEED.md rests on, from the sources."""
import re
import unittest
from pathlib import Path


def text(path):
    return Path(path).read_text()


VM = text("kernel/xnu-10063.141.1/osfmk/arm64/arm_vm_init.c")
INIT = text("kernel/xnu-10063.141.1/osfmk/arm/arm_init.c")
PRNG = text("kernel/xnu-10063.141.1/osfmk/prng/prng_random.c")
PE_GEN = text("kernel/xnu-10063.141.1/pexpert/gen/pe_gen.c")
LOADER = text("boot/loader/loader.c")
ROOT_BOOT = text("diagnostics/switch/SwitchRootBoot.c")
TEGRA_SE = text("drivers/TegraSE/TegraSE.cpp")
BUILD = text("boot/BUILD.bazel")
BOARD = text("kits/SwitchBoard/SwitchBoard.h")


def row(name):
    return re.search(r"X\(%s,.*" % name, BOARD).group(0)


class Consumers(unittest.TestCase):
    def test_the_first_user_is_the_physmap_slide_in_arm_vm_init(self):
        self.assertLess(VM.index('switch_diag_early_stage(17,'), VM.index("early_random() & ARM64_PHYSMAP_SLIDE_MASK"))
        self.assertLess(VM.index("early_random() & ARM64_PHYSMAP_SLIDE_MASK"), VM.index('switch_diag_early_stage(18,'))

    def test_the_stack_guard_comes_after_arm_vm_init_is_entered(self):
        self.assertLess(INIT.index('switch_diag_early_stage(16,'), INIT.index("__stack_chk_guard = (unsigned long)early_random()"))

    def test_the_seed_is_read_once_and_cleared(self):
        self.assertIn('"random-seed"', PE_GEN)
        self.assertIn("bootseed_init();", PRNG)
        self.assertIn("cc_clear(sizeof(bootseed), bootseed);", PRNG)


class Gates(unittest.TestCase):
    def test_the_loader_mixes_only_with_the_se2_feature(self):
        self.assertRegex(LOADER, r"if \(switch_board_has_feature\(board, SWITCH_FEATURE_SE2\)\)\s+\(void\)switch_seed_mix")

    def test_the_kernel_seed_step_is_behind_the_same_feature(self):
        self.assertIn("{SWITCH_FEATURE_SE2, tegra_se_start,", ROOT_BOOT)
        self.assertIn("write_random(seed, sizeof seed)", TEGRA_SE)

    def test_the_erista_row_has_no_features(self):
        fields = [f.strip() for f in row("ERISTA").split(",")]
        self.assertEqual(fields[5], "0")  # X(id, name, t210b01, sku, boot, features, ...)

    def test_the_package_pins_one_seed_for_both_boards(self):
        self.assertEqual(len(re.findall(r"_ADT_SEED,", BUILD)), 2)
        self.assertEqual(len(re.search(r'_ADT_SEED = "([0-9a-f]+)"', BUILD).group(1)), 512)


if __name__ == "__main__":
    unittest.main()
