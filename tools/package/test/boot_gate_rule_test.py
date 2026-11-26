"""boot/defs.bzl and boot/BUILD.bazel: how a package may enable a boot row, read as text (Bazel is not run).

The conditions, the three users of one derived table, and the targets that may not use it.
"""

import re
import unittest
from pathlib import Path

DEFS = Path('boot/defs.bzl').read_text()
BUILD = Path('boot/BUILD.bazel').read_text()


def function(name: str) -> str:
    match = re.search(rf'^def {name}\(.*?(?=^def |^[a-z_]+ = rule\()', DEFS, re.MULTILINE | re.DOTALL)
    assert match, name
    return match.group(0)


def target(name: str) -> str:
    match = re.search(rf'^boot_entry\(\n    name = "{name}",.*?^\)', BUILD, re.MULTILINE | re.DOTALL)
    assert match, name
    return match.group(0)


DERIVE = function('_derive_boards')
IMPL = function('_boot_package_impl')


class Allowlist(unittest.TestCase):
    def test_only_the_two_first_xnu_targets_may_enable_a_row_and_only_the_erista(self) -> None:
        table = re.search(r'_BOOT_ENABLE_TARGETS = \{(.*?)\n\}', DEFS, re.DOTALL).group(1)
        self.assertEqual(
            [('erista_first_xnu', 'erista'), ('erista_first_xnu_seeded', 'erista')],
            re.findall(r'"(\w+)": \["(\w+)"\]', table),
        )

    def test_the_other_packages_do_not_name_the_attribute(self) -> None:
        for name in ('package', 'erista_package', 'erista_rows_work'):
            self.assertNotIn('enable_boot_rows', target(name), name)
        self.assertEqual(2, len(re.findall(r'enable_boot_rows = select\(', BUILD)))
        self.assertEqual(2, len(re.findall(r'enable_boot_rows', BUILD)))

    def test_the_two_targets_enable_it_only_behind_the_off_by_default_flag(self) -> None:
        for name in ('erista_first_xnu', 'erista_first_xnu_seeded'):
            text = target(name)
            self.assertIn(':erista_boot_run_on": ["erista"]', text)
            self.assertIn('"//conditions:default": []', text)
        flag = re.search(r'bool_flag\(\n    name = "erista_boot_run",\n    build_setting_default = (\w+),', BUILD)
        self.assertEqual('False', flag.group(1))
        self.assertIn('flag_values = {":erista_boot_run": "True"}', BUILD)

    def test_the_attribute_defaults_to_nothing(self) -> None:
        self.assertRegex(DEFS, r'"enable_boot_rows": attr\.string_list\(\n')
        self.assertNotRegex(DEFS, r'"enable_boot_rows": attr\.string_list\([^)]*default')


class Conditions(unittest.TestCase):
    def test_each_condition_fails_the_analysis(self) -> None:
        for condition, reason in (
            ('not release and not prebuilt', 'a release profile or prebuilt loader'),
            ('ctx.attr.require_early_trace', 'no require_early_trace'),
            ('early_trace_check != None', 'no early-trace stamp'),
            ('ctx.attr.stop_after_vm', 'no stop_after_vm'),
            ('not smp', 'more than one core'),
        ):
            self.assertIn(f'({condition}, "{reason}")', DERIVE, condition)
        self.assertIn('fail("%s enables boot rows but has %s"', DERIVE)

    def test_an_unlisted_target_or_package_fails(self) -> None:
        self.assertIn('_BOOT_ENABLE_TARGETS.get(ctx.label.name) != rows or ctx.label.package != "boot"', DERIVE)

    def test_the_source_must_be_the_real_board_table(self) -> None:
        self.assertIn('ctx.attr.boards.label.package != _BOARD_TABLE_PACKAGE', DERIVE)
        self.assertIn('_BOARD_TABLE_PACKAGE = "kits/SwitchBoard"', DEFS)
        self.assertIn('_BOARD_TABLE_NAME = "SwitchBoard.h"', DEFS)

    def test_the_tool_is_told_every_condition_again(self) -> None:
        self.assertIn(
            'args.add_all(["--early-trace", "--stop-after-vm", "--single-core", "--profile", "debug"])', DERIVE
        )
        self.assertIn('args.add("--board", rows[0])', DERIVE)
        self.assertIn('args.add("--trace-stamp", early_trace_check)', DERIVE)

    def test_only_a_debug_profile_is_ever_passed(self) -> None:
        self.assertNotIn('"release"', DERIVE.replace('a release profile', ''))

    def test_the_existing_conditions_stay(self) -> None:
        self.assertIn('stop_after_vm needs require_early_trace', IMPL)
        self.assertIn('needs the debug profile', IMPL)


class OneTableForAllThree(unittest.TestCase):
    def test_the_derived_table_is_made_once_and_replaces_the_real_one_everywhere(self) -> None:
        self.assertEqual(1, IMPL.count('_derive_boards('))
        self.assertIn('markers = _swap(markers, ctx.file.boards, boards)', IMPL)
        self.assertIn('loader_headers = _swap(loader_headers, ctx.file.boards, boards)', IMPL)
        self.assertEqual(1, IMPL.count('ctx.files.loader_include_markers'))
        self.assertEqual(1, IMPL.count('ctx.files.loader_hdrs'))

    def test_the_loader_is_compiled_with_those_lists(self) -> None:
        self.assertIn('_compile_loader(ctx, out, profile, layout_h, runs_h, constants, markers, loader_headers)', IMPL)
        compile_loader = function('_compile_loader')
        self.assertIn('for marker in markers:', compile_loader)
        self.assertIn('[src] + headers +', compile_loader)
        self.assertNotIn('ctx.files.loader_include_markers', compile_loader)
        self.assertNotIn('ctx.files.loader_hdrs', compile_loader)

    def test_the_script_gets_the_same_table(self) -> None:
        self.assertIn('args.add("--boards", boards)', IMPL)
        self.assertNotIn('args.add("--boards", ctx.file.boards)', IMPL)
        self.assertIn('ctx.file.dtb, boards], [script_text, script]', IMPL)

    def test_the_agreement_check_is_an_input_of_bl33(self) -> None:
        self.assertIn('args.add("--derived", boards)', IMPL)
        self.assertIn('args.add("--script", script_text)', IMPL)
        self.assertIn('+ loader_constants + board_checks + ', IMPL)
        self.assertIn('board_checks.append(agreed)', IMPL)

    def test_the_swap_needs_the_real_table_exactly_once(self) -> None:
        swap = function('_swap')
        self.assertIn('!= 1', swap)
        self.assertIn('fail(', swap)

    def test_the_prebuilt_loader_never_sees_a_derived_table(self) -> None:
        self.assertIn('not release and not prebuilt', DERIVE)
        self.assertIn('prebuilt = bool(ctx.attr.prebuilt_loader)', IMPL)

    def test_the_derive_action_reads_only_the_real_table(self) -> None:
        self.assertIn('[real, early_trace_check], [derived], "BoardHeader"', DERIVE)


class TheTraceStampGatesEverything(unittest.TestCase):
    def test_the_stamp_is_made_before_the_table_and_passed_to_it(self) -> None:
        self.assertLess(IMPL.index('"EarlyTraceCheck"'), IMPL.index('_derive_boards('))
        self.assertIn('_derive_boards(ctx, out, release, smp, prebuilt, early_trace_check)', IMPL)
        self.assertEqual(1, IMPL.count('"EarlyTraceCheck"'))

    def test_bl33_reads_the_stamp_directly_only_when_it_enables_rows(self) -> None:
        self.assertIn('board_checks + ([early_trace_check] if ctx.attr.enable_boot_rows else []), [bl33]', IMPL)

    def test_the_package_still_reads_the_stamp(self) -> None:
        self.assertIn('([early_trace_check] if early_trace_check else [])', IMPL)

    def test_no_other_action_gains_the_stamp(self) -> None:
        mnemonics = sorted(
            m for line in DEFS.splitlines() if 'early_trace_check' in line for m in re.findall(r'"([A-Z]\w+)"', line)
        )
        self.assertEqual(['BoardHeader', 'BootBl33', 'EarlyTraceCheck'], mnemonics)
        self.assertEqual(1, IMPL.count('[early_trace_check] if early_trace_check else []'))

    def test_the_derive_action_fails_when_the_stamp_action_fails(self) -> None:
        self.assertIn('[real, early_trace_check], [derived]', DERIVE)
        self.assertIn('"--stamp", early_trace_check.path], [ctx.file.kernel], [early_trace_check]', IMPL)


if __name__ == '__main__':
    unittest.main()
