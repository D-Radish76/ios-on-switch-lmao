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
        phrases = (
            'implemented offline',
            'is unchanged',
            'is not approved',
            'it does not approve a run',
            'Neither is given',
        )
        for phrase in phrases:
            self.assertIn(phrase.lower(), GATE.lower(), phrase)
        self.assertTrue((ROOT / 'tools/package/board_header.py').exists())


class MacResultsMacho(unittest.TestCase):
    def test_the_macho_report_and_the_seeded_commands_are_recorded(self) -> None:
        text = read('ERISTA_FIRST_XNU_MAC.md')
        for phrase in (
            '2a13ebdc2',
            '`__LINKEDIT` ends at `0x1829ce8`, the file at `0x386a000`',
            '3 empty segments',
            '`0x1f4000`',
            '`0x2318`',
            '5 segments have file offsets',
            'erista_seed.py generate',
            'git ls-files --error-unmatch local/erista-seed.hex',
            'erista_seeded_build.py build',
            'erista_seeded_build.py clean --seed',
            'erista_first_xnu_seeded_work/loader.uboot.bin',
            'cannot boot the Erista',
            'Report the fingerprint, exit codes and hashes only',
        ):
            self.assertIn(phrase, text, phrase)
        self.assertNotIn('--erista_boot_run=true\npython3', text)


class SeededWrapperRecord(unittest.TestCase):
    def test_the_mac_measurements_and_the_wrapper_changes_are_recorded(self) -> None:
        text = read('ERISTA_FIRST_XNU_MAC.md')
        section = text[text.index('## 12. Mac results for the seeded build') :]
        for phrase in (
            '`4b7779b2`',
            '652.589 s',
            '13046 actions',
            '`flat_bytes` 122880',
            '`0xa001e000`',
            '`0xf00` (3840)',
            '755',
            'any user of the Mac can read the seeded outputs',
            '`--disk_cache=`, which wins over line 19',
            'chmod 700',
            '--bes_backend=',
            'It does not go by a path containing "seeded"',
            'erista-full-ram-seeded.adt',
            '`--deep` adds `bazel clean`',
            'never follows a link',
            'Not detected: compressed or encrypted data',
            'scan --caches',
            'a "disk cache hit" or "remote cache hit" there is a failure',
        ):
            self.assertIn(phrase, section, phrase)
        self.assertNotIn('erista_boot_run=true', section)
        self.assertIn('section 12', read('ERISTA_RANDOM_SEED.md'))


class MacResults(unittest.TestCase):
    def test_the_mac_report_and_the_graph_check_are_recorded(self) -> None:
        for phrase in (
            'c8f428d0b',
            'Six tests passed',
            'No boot=1 package was built',
            'Action graph check',
            'LoaderCompile',
            'boards.ok',
            'early-trace.ok',
            'first_xnu_check.py',
            'bazel-bin/kernel/kernel.macho',
        ):
            self.assertIn(phrase, MAC, phrase)
        for mnemonic in (
            'BoardHeader',
            'BoardAgree',
            'BootScript',
            'LoaderCompile',
            'BootBl33',
            'BootPackage',
            'EarlyTraceCheck',
        ):
            self.assertIn(f'"{mnemonic}"', DEFS, mnemonic)
            self.assertIn(mnemonic, MAC, mnemonic)

    def test_the_stamp_gap_is_closed_and_said_so(self) -> None:
        self.assertIn('is closed in the next commit', MAC)
        self.assertIn('--trace-stamp', MAC)
        self.assertIn('closed offline', GATE)
        self.assertIn('--trace-stamp', DEFS)
        self.assertIn('early_trace_check]', DEFS)

    def test_the_linker_warning_is_explained_from_the_flags_and_nothing_is_changed(self) -> None:
        for phrase in (
            '-new_linker is obsolete',
            '__PLK_LINKEDIT',
            'MakeInc.def',
            'Eight of the eight match',
            'the link flags are not changed',
            'xcrun ld -v',
        ):
            self.assertIn(phrase, MAC, phrase)
        recipe = read('kernel/xnu-10063.141.1/config/recipes_development.bzl')
        for segment in (
            '__PRELINK_TEXT',
            '__PRELINK_INFO',
            '__PLK_TEXT_EXEC',
            '__PRELINK_DATA',
            '__PLK_DATA_CONST',
            '__PLK_LLVM_COV',
            '__PLK_LINKEDIT',
            '__LINKINFO',
        ):
            self.assertRegex(recipe, rf'-sectcreate,"?{segment}"?,', segment)
            self.assertNotIn(segment, re.search(r'-Wl,-segment_order,(\S+)"', recipe).group(1), segment)
        self.assertIn('-Wl,-new_linker', recipe)

    def test_the_held_options_are_listed_with_their_risk(self) -> None:
        run = read('ERISTA_FIRST_XNU_RUN.md')
        for phrase in (
            'Held: the run condition for trickling marks',
            'A total time limit',
            'A shorter wait after VM done',
            'Fan control, or a hang protection independent of the kernel',
            'none a safety guarantee',
            'THERMTRIP, WDT, fan and voltage operations stay forbidden',
        ):
            self.assertIn(phrase, run, phrase)

    def test_trickling_marks_are_not_called_a_safety_limit(self) -> None:
        run = read('ERISTA_FIRST_XNU_RUN.md')
        for phrase in (
            'Marks that keep trickling in',
            '21 x 30 s',
            'It is not a safety limit',
            'the PSCI reset after XNU owns the core is untried',
        ):
            self.assertIn(phrase, run, phrase)


class MeasuredKernel(unittest.TestCase):
    def test_the_mac_kernel_results_and_the_corrected_path_are_recorded(self) -> None:
        for phrase in (
            '## 9. Mac results for the kernel',
            'bazel-bin/kernel/kernel.macho',
            'Apple ld-27106.1',
            'Xcode 27.2',
            'exit 0',
            'exit 1',
            '`__LINKEDIT` is last',
            'Mach-O measured by the Mac',
            '0xfffffff004004000',
            '0xfffffff00427c000',
            'first_xnu_check.py macho',
            '0xfffffff007004000',
        ):
            self.assertIn(phrase, MAC, phrase)
        self.assertNotIn('kernel bazel-bin/kernel/prelinked', MAC + read('ERISTA_FIRST_XNU_BOOT.md'))

    def test_the_reported_virtual_entry_maps_to_the_reported_physical_entry(self) -> None:
        layout = read('kernel/switch/include/switch/boot_layout.h')
        self.assertIn('#define SWITCH_KERNEL_PHYS_BASE 0xb207c000', layout)
        physical = 0xB207C000 + (0xFFFFFFF0075CC4C8 - 0xFFFFFFF00427C000)
        self.assertEqual(0xB53CC4C8, physical)
        self.assertEqual(0, (0xFFFFFFF00427C000 - 0xB207C000) % 0x200000)
        self.assertIn('0xb53cc4c8', MAC)
        self.assertIn('0x182a000', MAC)

    def test_the_t3_t4_reuse_review_keeps_the_holds_and_the_tag(self) -> None:
        text = read('ERISTA_T3T4_REUSE.md')
        for phrase in (
            'stay on HOLD',
            'undecided',
            'v6.5.5',
            'Not read: `soc/timer.h`',
            'SOC_THERM and hang protection, evaluated separately',
            'BDK_HW_EXTRA_DEINIT',
            '0x9802D131',
            '0x049102CA',
            'not a system reset',
            'is not treated as a safety guarantee',
            'Neither is a precondition of the other',
            'both stay on HOLD',
        ):
            self.assertIn(phrase, text, phrase)
        self.assertIn('ERISTA_T3T4_REUSE.md', BLOCKERS)

    def test_the_expected_addresses_and_permissions_are_the_link_lines(self) -> None:
        recipe = read('kernel/xnu-10063.141.1/config/recipes_development.bzl')
        self.assertIn('-Wl,-segaddr,__PRELINK_TEXT,0xfffffff004004000', recipe)
        self.assertIn('-Wl,-image_base,0xfffffff007004000', recipe)
        for segment, protection in (
            ('__TEXT', 'r--'),
            ('__TEXT_EXEC', 'r-x'),
            ('__KLD', 'r-x'),
            ('__LAST', 'r-x'),
            ('__LASTDATA_CONST', 'r--'),
            ('__DATA_CONST', 'r--'),
            ('__KLDDATA', 'rw-'),
            ('__LINKINFO', 'r--'),
        ):
            self.assertIn(f'-Wl,-segprot,{segment},{protection},{protection}', recipe, segment)

    def test_the_seed_review_and_the_wrapper_finding_are_recorded(self) -> None:
        for phrase in (
            'Final review of the seed path',
            'Closed (2026-11-27)',
            'WRONG_BOARD',
            'from the repository root',
        ):
            self.assertIn(phrase, MAC, phrase)

    def test_the_t3_t4_review_keeps_the_holds(self) -> None:
        text = read('ERISTA_T3T4_MINIMAL.md')
        for phrase in (
            'Nothing is built, approved or waived',
            'stay on HOLD',
            'undecided',
            'the smallest T4 is the PMIC watchdog alone',
            'not an overheat protection',
            "the person's POWER hold is the\nonly protection",
        ):
            self.assertIn(phrase, text, phrase)
        self.assertIn('ERISTA_T3T4_MINIMAL.md', BLOCKERS)


class Blockers(unittest.TestCase):
    def test_t2_is_done_and_the_list_still_has_nine_steps(self) -> None:
        self.assertIn('| 4 | T2 | done: PASS | no |', BLOCKERS)
        self.assertEqual([str(n) for n in range(1, 11)], re.findall(r'^\| (\d+) \|', BLOCKERS, re.MULTILINE))
        self.assertIn('ERISTA_FIRST_XNU_BOOT_GATE.md', BLOCKERS)
        self.assertIn('ERISTA_FIRST_XNU_RUN.md', BLOCKERS)

    def test_the_t3_t4_waiver_is_still_open(self) -> None:
        self.assertIn('| 7 | T3 and T4 built and proved, or waived | open.', BLOCKERS)


if __name__ == '__main__':
    unittest.main()
