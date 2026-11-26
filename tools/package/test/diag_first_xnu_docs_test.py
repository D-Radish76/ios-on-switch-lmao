"""The first-XNU run sheet and the boot-gate design against the sources they quote."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')


def read(name: str) -> str:
    return (ROOT / name).read_text()


RUN = read('ERISTA_FIRST_XNU_RUN.md')
GATE = read('ERISTA_FIRST_XNU_BOOT_GATE.md')
BLOCKERS = read('ERISTA_FIRST_XNU_BLOCKERS.md')
MAC = read('ERISTA_FIRST_XNU_MAC.md')
BOOT_DOC = read('ERISTA_FIRST_XNU_BOOT.md')
VM_DONE = read('diagnostics/switch/SwitchVmDone.c')
DEFS = read('boot/defs.bzl')


class RunSheet(unittest.TestCase):
    def test_the_screen_texts_it_quotes_are_the_kernels(self) -> None:
        for text in (
            'XNU VM INITIALIZED',
            'ARM_VM_INIT RETURNED AFTER STAGE 21',
            'RESETTING IN',
            'HOLD POWER TO STOP',
            'SMC RETURNED',
            'NO RESET',
        ):
            self.assertIn(text, RUN, text)
            self.assertIn(text, VM_DONE, text)

    def test_the_stop_limits_are_the_boot_documents(self) -> None:
        for phrase in ('No new mark for 30 s', 'n + 10 s'):
            self.assertIn(phrase, RUN)
            self.assertIn(phrase, BOOT_DOC)
        self.assertIn('provisional', RUN)

    def test_the_margin_figures_are_the_mac_checklists(self) -> None:
        for figure in ('122880', '`0xf00`', '`0x1000`', '0xa001ef00'):
            self.assertIn(figure, RUN + BOOT_DOC)
        for figure in ('122880', '0xf00', '0x1000'):
            self.assertIn(figure, MAC)

    def test_the_holds_and_decisions_are_stated(self) -> None:
        for phrase in (
            'Nothing here approves it',
            '`boot` stays 0 and the feature mask stays 0',
            'T3, T4, the full D1 and every other XNU run stay on HOLD',
            'The T3/T4 waiver is undecided',
            'D1 is not a condition',
            'No autonomous hang',
            'A second run needs a new written approval',
            "No result of this run shows XNU's SD driver",
        ):
            self.assertIn(phrase, RUN, phrase)

    def test_the_documents_it_points_to_exist(self) -> None:
        for name in sorted(set(re.findall(r'`(ERISTA_[A-Z0-9_]+\.md)`', RUN + GATE))):
            self.assertTrue((ROOT / name).exists(), name)

    def test_no_em_dash(self) -> None:
        for text in (RUN, GATE):
            self.assertNotIn('—', text)


class StopRules(unittest.TestCase):
    def test_the_120_second_limit_is_withdrawn_everywhere(self) -> None:
        self.assertIn('120 s limit from the loader', RUN)
        self.assertIn('withdrawn', RUN)
        for name, text in (('BOOT', BOOT_DOC), ('BLOCKERS', BLOCKERS), ('MAC', MAC)):
            self.assertNotRegex(text, r'120 s (total|since|after)', name)
            self.assertNotIn('30 s and 120 s', text, name)
        self.assertIn('withdrawn', BOOT_DOC)

    def test_the_vm_text_is_drawn_once_so_the_sheet_says_it_does_not_count_down(self) -> None:
        self.assertIn('does not count down', RUN)
        self.assertEqual(1, VM_DONE.count('"RESETTING IN   S. HOLD POWER TO STOP"'))
        self.assertEqual(1, VM_DONE.count('RESETTING IN'))

    def test_the_reset_wait_is_the_setting_the_sheet_names(self) -> None:
        self.assertIn('"vm_reset_seconds": attr.int(default = 15', DEFS)
        self.assertIn('vm_reset_seconds < 1 or ctx.attr.vm_reset_seconds > 60', DEFS)
        self.assertIn('`vm_reset_seconds`, 15 unless the owner sets it, 1 to 60', RUN)
        self.assertIn('n + 10', RUN)

    def test_no_total_limit_is_claimed_to_be_safe(self) -> None:
        self.assertIn('Nothing measured says', RUN)
        self.assertIn('neither a thermal limit', BOOT_DOC)


class Seed(unittest.TestCase):
    SEED = read('tools/package/erista_seed.py')
    WRAPPER = read('tools/package/erista_seeded_build.py')

    def section(self) -> str:
        return MAC[MAC.index('## 6. The seed and its secrecy') : MAC.index('## 7. The full command set')]

    def test_the_commands_come_in_the_documented_order(self) -> None:
        text = self.section()
        order = [
            'erista_seed.py generate',
            'erista_seed.py check',
            'erista_seeded_build.py build',
            'erista_seeded_build.py scan',
            'shasum -a 256 bazel-bin/boot/erista-first-xnu-seeded',
            'erista_seeded_build.py clean      ',
            'erista_seeded_build.py clean --seed',
        ]
        positions = [text.index(item) for item in order]
        self.assertEqual(sorted(positions), positions)

    def test_the_wrapper_is_the_only_build_command_and_has_the_flags_the_section_names(self) -> None:
        text = self.section()
        self.assertNotRegex(text, r'^bazel build', 'a plain bazel build of the seeded package')
        for flag in (
            "'--config=ios17'",
            "'--//install:prebuilt=local'",
            "'--//kernel:early_trace=true'",
            "'--remote_cache='",
            "'--disk_cache='",
        ):
            self.assertIn(flag, self.SEED)
        self.assertIn("TARGET = '//boot:erista_first_xnu_seeded'", self.SEED)
        for flag in ('--profile', '--subcommands', '--build_event_json_file', '--execution_log_json_file'):
            self.assertIn(flag, self.WRAPPER)

    def test_the_section_says_what_scan_reads_and_the_secrecy_rules(self) -> None:
        text = self.section()
        for phrase in (
            "git's tracked files",
            'nothing else',
            'the 8-digit fingerprint',
            'is not shared',
            'Never: commit, upload, paste or log the seed',
        ):
            self.assertIn(phrase, text.replace("git's", "git's"), phrase)

    def test_the_tool_prints_only_a_fingerprint_and_the_seed_stays_ignored(self) -> None:
        self.assertIn("f'seed written to {path} (fingerprint {fingerprint(load(path))})", self.SEED)
        self.assertIn('erista-seed.hex', read('local/.gitignore'))
        self.assertFalse((ROOT / 'local/erista-seed.hex').exists())

    def test_the_run_sheet_and_the_blockers_name_the_seeded_package(self) -> None:
        self.assertIn('//boot:erista_first_xnu_seeded', RUN)
        self.assertIn('the owner prefers the per-install seed', BLOCKERS)


class BootGate(unittest.TestCase):
    def test_the_erista_row_is_still_boot_zero_with_no_features(self) -> None:
        self.assertIn('X(ERISTA, "erista", 0, 0, 0, 0,', read('kits/SwitchBoard/SwitchBoard.h'))

    def test_the_three_readers_it_names_are_in_the_sources(self) -> None:
        self.assertRegex(read('boot/loader/loader.c'), r'if \(!board->boot\)\s*refuse\(0x306\);')
        self.assertIn('(boot) ? switch_carveout_##id : NULL', ' '.join(read('boot/loader/carveout.h').split()))
        script = read('tools/package/boot_script.py')
        self.assertIn('def board_gate(header: str)', script)
        self.assertIn('if boot:', script)

    def test_the_kernel_table_ignores_the_flag(self) -> None:
        self.assertRegex(
            read('kits/SwitchBoard/SwitchBoardKernel.h'),
            r'SWITCH_KERNEL_ROW\(id, name, t210b01, sku, boot, features, model, compatible\) '
            r'SWITCH_KERNEL_FEATURES_##id,',
        )

    def test_the_rule_takes_the_table_per_target(self) -> None:
        for attribute in (
            '"boards": attr.label(',
            '"loader_include_markers": attr.label_list(',
            '"loader_hdrs": attr.label_list(',
        ):
            self.assertIn(attribute, DEFS)
        for attribute in ('`boards`', '`loader_include_markers`', '`loader_hdrs`'):
            self.assertIn(attribute, GATE)

    def test_stop_after_vm_already_needs_the_trace_build(self) -> None:
        self.assertIn('stop_after_vm needs require_early_trace', DEFS)

    def test_the_document_says_what_is_not_built(self) -> None:
        phrases = ('implemented offline', 'is unchanged', 'is not approved', 'it does not approve a run', 'Neither is given')
        for phrase in phrases:
            self.assertIn(phrase.lower(), GATE.lower(), phrase)
        self.assertTrue((ROOT / 'tools/package/board_header.py').exists())


class Blockers(unittest.TestCase):
    def test_t2_is_done_and_the_list_still_has_nine_steps(self) -> None:
        self.assertIn('| 4 | T2 | done: PASS | no |', BLOCKERS)
        self.assertEqual([str(n) for n in range(1, 10)], re.findall(r'^\| (\d) \|', BLOCKERS, re.MULTILINE))
        self.assertIn('ERISTA_FIRST_XNU_BOOT_GATE.md', BLOCKERS)
        self.assertIn('ERISTA_FIRST_XNU_RUN.md', BLOCKERS)

    def test_the_t3_t4_waiver_is_still_open(self) -> None:
        self.assertIn('| 7 | T3 and T4 built and proved, or waived | open.', BLOCKERS)


if __name__ == '__main__':
    unittest.main()
