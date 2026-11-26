"""The first-XNU observation's end (SwitchVmDone.c): where it is called, what it may touch, and that no other config has it."""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path('.')
sys.path.insert(0, str(ROOT / 'tools/package'))
import boot_args

DONE = (ROOT / 'diagnostics/switch/SwitchVmDone.c').read_text()
DIAG = (ROOT / 'diagnostics/switch/SwitchDiag.c').read_text()
HEADER = (ROOT / 'diagnostics/switch/SwitchDiag.h').read_text()
BUILD = (ROOT / 'diagnostics/switch/BUILD.bazel').read_text()
ARM_INIT = (ROOT / 'kernel/xnu-10063.141.1/osfmk/arm/arm_init.c').read_text()
PACKAGE_BUILD = (ROOT / 'boot/BUILD.bazel').read_text()
DEFS = (ROOT / 'boot/defs.bzl').read_text()


class VmDoneTest(unittest.TestCase):
    def test_boot_args(self):
        plain = boot_args.command_line(False)
        self.assertEqual(plain, boot_args.APPLE_KEYSTORE)
        self.assertEqual(boot_args.command_line(False, False, True), plain + ' switch_stop_after_vm=1')
        with self.assertRaises(ValueError):
            boot_args.command_line(False, True, True)

    def test_only_the_first_xnu_targets_bake_the_stop_in(self):
        targets = re.findall(r'boot_entry\(\n    name = "(\w+)",.*?\n\)', PACKAGE_BUILD, re.S)
        for name in targets:
            block = PACKAGE_BUILD[PACKAGE_BUILD.index(f'name = "{name}"') :].split('\n)\n')[0]
            self.assertEqual('stop_after_vm = True' in block, name in ('erista_first_xnu', 'erista_first_xnu_seeded'), name)
        self.assertIn('stop_after_vm needs require_early_trace', DEFS)

    def test_the_code_is_debug_only_and_linked_only_there(self):
        self.assertEqual(DONE.count('#if DEVELOPMENT || DEBUG'), 1)
        self.assertTrue(DONE.rstrip().endswith('#endif'))
        release_driver = BUILD[BUILD.index('name = "SwitchDiag",') : BUILD.index('name = "SwitchDiagDebug"')]
        self.assertNotIn('SwitchVmDone.c', release_driver)
        self.assertIn('"SwitchVmDone.c"', BUILD[BUILD.index('name = "SwitchDiagDebug"') :])
        self.assertRegex(HEADER, r'#define switch_diag_vm_done\(\)\s+\(\(void\)0\)')
        self.assertIn('unsigned switch_diag_vm_stage21;', DIAG[DIAG.index('#if DEVELOPMENT || DEBUG\n// Set when') :][:300])

    def test_stage_21_and_the_return_are_told_apart(self):
        self.assertIn('stage == 21 && switch_early_record[0] == 2', DIAG)
        self.assertIn('if (!switch_diag_vm_stage21)', DONE)
        at = ARM_INIT.index('arm_vm_init(xmaxmem, args);')
        self.assertEqual(ARM_INIT.count('switch_diag_vm_done();'), 1)
        self.assertGreater(ARM_INIT.index('switch_diag_vm_done();'), ARM_INIT.index('boot_progress_stage("arm_vm_init returned");'))
        self.assertLess(ARM_INIT.index('switch_diag_vm_done();'), ARM_INIT.index('PE_init_platform(TRUE'))

    def test_it_touches_no_firmware_and_no_hardware_beyond_the_framebuffer(self):
        code = re.sub(r'/\*.*?\*/', '', DONE, flags=re.S).lower()
        for word in ('smc', 'hvc', 'psci', 'switch_mmio', 'tegra_', 'iolog', 'sdmmc', 'pmc'):
            self.assertNotIn(word, code, word)
        self.assertEqual(re.findall(r'ml_io_map\(\(vm_offset_t\)(\w+)', DONE), ['SWITCH_FB_PA'])
        self.assertIn('msr daifset, #15', DONE)
        self.assertIn('wfi', DONE)

    def test_the_stop_is_documented_as_not_safe(self):
        self.assertIn('not a safe stop', DONE)
        doc = (ROOT / 'ERISTA_FIRST_XNU_BOOT.md').read_text()
        self.assertIn('Not a safe stop', doc)
        self.assertIn('Hardware boot is not allowed', doc)

    def test_it_does_not_clear_the_marks(self):
        self.assertEqual(re.findall(r'band\(pixels, (\d+), (\d+)\)', DONE), [('20', '150')])
        self.assertLess(150, 280)


if __name__ == '__main__':
    unittest.main()
