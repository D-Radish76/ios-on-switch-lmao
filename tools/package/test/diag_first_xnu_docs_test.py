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
        for phrase in ('No new mark for 30 s', '120 s'):
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
        for phrase in ('none of it done', 'Nothing here changes a row or a build rule', 'it does not approve a run'):
            self.assertIn(phrase.lower(), GATE.lower(), phrase)
        self.assertFalse((ROOT / 'tools/package/board_header.py').exists())


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
