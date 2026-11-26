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
        self.assertEqual(boot_args.command_line(False, False, True), plain + ' switch_stop_after_vm=15')
        self.assertEqual(boot_args.command_line(False, False, 60), plain + ' switch_stop_after_vm=60')
        self.assertEqual(boot_args.command_line(False, False, 1), plain + ' switch_stop_after_vm=1')
        for bad in (0, -1, 61, 1000):
            with self.assertRaises(ValueError):
                boot_args.command_line(False, False, bad)
        with self.assertRaises(ValueError):
            boot_args.command_line(False, True, True)

    def test_the_command_line_flag_parser_is_strict(self):
        self.assertEqual(boot_args.parse_flags(['--stop-after-vm']), (False, False, True))
        self.assertEqual(boot_args.parse_flags(['--smp', '--stop-after-vm=7']), (True, False, 7))
        for bad in (['--stop-after-vm=', '--x'], ['--stop-after-vm=abc'], ['--stop-after-vm=123'], ['--stop-after-vm', '--stop-after-vm=5'],
                    ['--stop-after-vm=-1'], ['--stop-after-vm=٣']):
            with self.assertRaises(SystemExit):
                boot_args.parse_flags(bad)

    def test_the_package_rule_passes_the_seconds_and_checks_the_range(self):
        self.assertIn('"--stop-after-vm=%d" % ctx.attr.vm_reset_seconds', DEFS)
        self.assertIn('vm_reset_seconds is 1 to 60', DEFS)
        self.assertIn('"vm_reset_seconds": attr.int(default = 15', DEFS)
        self.assertNotIn('vm_reset_seconds', PACKAGE_BUILD)

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

    def test_it_touches_no_hardware_beyond_the_framebuffer_the_timer_and_one_firmware_call(self):
        code = re.sub(r'/\*.*?\*/', '', DONE, flags=re.S).lower()
        for word in ('hvc', 'switch_mmio', 'tegra_', 'iolog', 'sdmmc', 'pmc', 'wdt', 'thermtrip', 'readl', 'writel'):
            self.assertNotIn(word, code, word)
        self.assertEqual(code.count('smc #0'), 1)
        self.assertEqual(DONE.count('SWITCH_PSCI_SYSTEM_RESET'), 1)
        self.assertEqual(set(re.findall(r'msr (\w+)[,\s]', DONE)), {'daifset', 'cntkctl_el1'})
        self.assertTrue(set(re.findall(r'mrs %0, (\w+)', DONE)) <= {'CurrentEL', 'cntfrq_el0', 'cntkctl_el1', 'cntpct_el0'})
        self.assertEqual(re.findall(r'ml_io_map\(\(vm_offset_t\)(\w+)', DONE), ['SWITCH_FB_PA'])
        self.assertIn('msr daifset, #15', DONE)
        self.assertIn('wfi', DONE)

    def test_the_reset_is_made_once_and_never_retried(self):
        self.assertEqual(DONE.count('smc'), 1)
        after = DONE[DONE.index('smc #0') :]
        self.assertIn('NO RETRY', after)
        self.assertLess(after.index('park();'), after.index('void switch_diag_vm_done'))
        for loop in re.findall(r'(for \(;;\)|while \(1\)|goto \w+)', DONE):
            self.assertEqual(loop, 'for (;;)')
        self.assertEqual(DONE.count('for (;;)'), 1)
        self.assertIn('static void park(void)', DONE[: DONE.index('for (;;)')])

    def test_the_wait_is_the_loaders_and_every_refusal_parks_without_a_call(self):
        self.assertIn('0xf4', DONE)
        self.assertIn('wfe', DONE)
        self.assertIn('switch_vm_reset_parse(PE_boot_args(), &seconds)', DONE)
        refusals = re.findall(r'NO RESET', DONE)
        self.assertGreaterEqual(len(refusals), 2)
        for needle in ('current_el != 4', '!ticks', '!counter_advances()'):
            self.assertIn(needle, DONE)
        self.assertLess(DONE.index('current_el != 4'), DONE.index('smc #0'))
        self.assertLess(DONE.index('SWITCH_VM_RESET_INVALID'), DONE.index('reset_after(pixels, seconds);'))

    def test_the_reset_is_a_vm_done_ending_not_a_protection(self):
        doc = (ROOT / 'ERISTA_FIRST_XNU_BOOT.md').read_text()
        self.assertIn('timed reset', doc)
        self.assertIn('Not a hang or temperature protection', doc)
        self.assertIn('Hardware boot stays held', doc)

    def test_it_does_not_clear_the_marks(self):
        self.assertEqual(re.findall(r'band\(pixels, (\d+), (\d+)\)', DONE), [('100', '150'), ('20', '150')])
        self.assertLess(150, 280)


if __name__ == '__main__':
    unittest.main()
