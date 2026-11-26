"""The derived board table: one field of one row changes, and only when every condition of the diagnostic is stated."""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from board_header import agree, derive, rows, verify
from boot_script import board_gate

HEADER_PATH = Path('kits/SwitchBoard/SwitchBoard.h')
HEADER = HEADER_PATH.read_text()
ERISTA_ROW = 'X(ERISTA, "erista", 0, 0, 0, 0,'
ALL = {'early_trace': True, 'stop_after_vm': True, 'single_core': True, 'profile': 'debug'}
TOOL = Path('tools/package/board_header.py')
CARVEOUT_PROBE = r"""
#include <stdio.h>
#include "carveout.h"
int main(void)
{
    for (size_t i = 0; i < sizeof(switch_boards) / sizeof(switch_boards[0]); ++i)
    {
        const struct switch_board *b = &switch_boards[i];
        printf("%s %d %d\n", b->name, b->boot, switch_carveout_for(b) != NULL);
    }
    const struct switch_board *detected = switch_board_detect(0x00022117, 0x00000204);
    printf("detected %s %d\n", detected->name, detected->boot);
    return 0;
}
"""


def refused(case: unittest.TestCase, **kwargs: object) -> str:
    with case.assertRaises(SystemExit) as raised:
        derive(kwargs.pop('header', HEADER), kwargs.pop('board', 'erista'), **{**ALL, **kwargs})
    case.assertIn('refused', str(raised.exception))
    return str(raised.exception)


class SourceHeader(unittest.TestCase):
    def test_the_original_header_keeps_the_erista_at_boot_0_with_no_features(self) -> None:
        self.assertIn(ERISTA_ROW, HEADER)
        erista = next(r for r in rows(HEADER) if r['name'] == 'erista')
        self.assertEqual(
            ('0', '0', '0', '0', '0'), (erista['t210b01'], erista['sku'], erista['boot'], erista['features'], '0')
        )
        oled = next(r for r in rows(HEADER) if r['name'] == 'oled')
        self.assertEqual('1', oled['boot'])


class Derive(unittest.TestCase):
    def test_only_the_erista_boot_field_changes(self) -> None:
        derived = derive(HEADER, 'erista', **ALL)
        self.assertEqual(len(HEADER), len(derived))
        changed = [i for i, (a, b) in enumerate(zip(HEADER, derived, strict=True)) if a != b]
        self.assertEqual(1, len(changed))
        self.assertEqual(('0', '1'), (HEADER[changed[0]], derived[changed[0]]))
        self.assertIn('X(ERISTA, "erista", 0, 0, 1, 0,', derived)
        self.assertEqual(HEADER.replace(ERISTA_ROW, 'X(ERISTA, "erista", 0, 0, 1, 0,'), derived)
        verify(HEADER, derived, 'erista')

    def test_the_oled_row_and_the_features_are_untouched(self) -> None:
        before, after = rows(HEADER), rows(derive(HEADER, 'erista', **ALL))
        for one, two in zip(before, after, strict=True):
            keep = {k: v for k, v in one.items() if k != 'boot'}
            self.assertEqual(keep, {k: v for k, v in two.items() if k != 'boot'})
            if one['name'] != 'erista':
                self.assertEqual(one['boot'], two['boot'])
        self.assertEqual('0', next(r for r in after if r['name'] == 'erista')['features'])

    def test_the_gate_gains_exactly_the_erista_clause(self) -> None:
        original, derived = board_gate(HEADER), board_gate(derive(HEADER, 'erista', **ALL))
        self.assertNotIn('erista', original)
        self.assertEqual(original, derived.split('# erista')[0].rstrip('\n'))
        self.assertIn('# erista\nif test "${t210b01}" = "0" -a "${sku}" = "0"; then', derived)

    def test_the_original_file_is_not_written(self) -> None:
        before = HEADER_PATH.read_bytes()
        derive(HEADER, 'erista', **ALL)
        self.assertEqual(before, HEADER_PATH.read_bytes())


class Refusals(unittest.TestCase):
    def test_release_is_refused(self) -> None:
        self.assertIn('release', refused(self, profile='release'))

    def test_a_missing_profile_is_refused(self) -> None:
        refused(self, profile='')

    def test_no_early_trace_is_refused(self) -> None:
        self.assertIn('early_trace', refused(self, early_trace=False))

    def test_no_stop_after_vm_is_refused(self) -> None:
        self.assertIn('stop_after_vm', refused(self, stop_after_vm=False))

    def test_more_than_one_core_is_refused(self) -> None:
        self.assertIn('core', refused(self, single_core=False))

    def test_an_unknown_board_is_refused(self) -> None:
        self.assertIn('unknown board', refused(self, board='switch2'))

    def test_the_oled_is_not_enableable_here(self) -> None:
        self.assertIn('unknown board', refused(self, board='oled'))

    def test_a_row_with_a_feature_is_refused(self) -> None:
        refused(self, header=HEADER.replace(ERISTA_ROW, 'X(ERISTA, "erista", 0, 0, 0, SWITCH_FEATURE_CHARGER,'))
        self.assertIn('features', refused(self, header=HEADER.replace(ERISTA_ROW, 'X(ERISTA, "erista", 0, 0, 0, 1,')))

    def test_a_header_that_already_boots_the_erista_is_refused(self) -> None:
        self.assertIn('already', refused(self, header=HEADER.replace(ERISTA_ROW, 'X(ERISTA, "erista", 0, 0, 1, 0,')))

    def test_a_changed_identity_is_refused(self) -> None:
        refused(self, header=HEADER.replace(ERISTA_ROW, 'X(ERISTA, "erista", 1, 0, 0, 0,'))
        refused(self, header=HEADER.replace(ERISTA_ROW, 'X(ERISTA, "erista", 0, 2, 0, 0,'))

    def test_a_missing_or_doubled_row_is_refused(self) -> None:
        refused(self, header=HEADER.replace('"erista"', '"icosa"'))
        row = next(line for line in HEADER.splitlines() if ERISTA_ROW in line)
        refused(self, header=HEADER.replace(row, row + ' \\\n' + row))

    def test_a_header_without_the_table_is_refused(self) -> None:
        refused(self, header=HEADER.replace('#define SWITCH_BOARDS(X)', '#define SWITCH_ROWS(X)'))

    def test_a_derived_header_with_another_change_is_refused(self) -> None:
        derived = derive(HEADER, 'erista', **ALL)
        with self.assertRaises(SystemExit):
            verify(HEADER, derived + '\n', 'erista')
        with self.assertRaises(SystemExit):
            verify(HEADER, derived.replace('"oled", 1, 3, 1,', '"oled", 1, 3, 0,'), 'erista')
        with self.assertRaises(SystemExit):
            verify(HEADER, HEADER, 'erista')

    def test_a_modified_original_makes_verify_fail(self) -> None:
        derived = derive(HEADER, 'erista', **ALL)
        with self.assertRaises(SystemExit):
            verify(HEADER.replace(ERISTA_ROW, 'X(ERISTA, "erista", 0, 0, 1, 0,'), derived, 'erista')


class ScriptAndLoaderAgree(unittest.TestCase):
    def test_a_script_made_from_the_derived_header_agrees_with_it(self) -> None:
        derived = derive(HEADER, 'erista', **ALL)
        agree(derived, board_gate(derived))
        agree(HEADER, board_gate(HEADER))

    def test_a_script_from_the_other_table_is_refused(self) -> None:
        derived = derive(HEADER, 'erista', **ALL)
        with self.assertRaises(SystemExit) as raised:
            agree(derived, board_gate(HEADER))
        self.assertIn('accepts', str(raised.exception))
        with self.assertRaises(SystemExit):
            agree(HEADER, board_gate(derived))

    def test_a_script_with_a_clause_removed_is_refused(self) -> None:
        derived = derive(HEADER, 'erista', **ALL)
        gate = board_gate(derived)
        with self.assertRaises(SystemExit):
            agree(derived, gate.replace('# oled', '# other'))

    def test_the_loader_s_carveout_table_follows_the_header_it_includes(self) -> None:
        compiler = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
        self.assertIsNotNone(compiler, 'a C compiler is needed to build the loader table probe')
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp, 'probe.c')
            probe.write_text(CARVEOUT_PROBE)
            outputs = {}
            for label, text in (('original', HEADER), ('derived', derive(HEADER, 'erista', **ALL))):
                directory = Path(tmp, label)
                directory.mkdir()
                (directory / 'SwitchBoard.h').write_text(text)
                binary = Path(tmp, f'probe-{label}')
                subprocess.run(
                    [compiler, '-std=c11', '-I', str(directory), '-I', 'boot/loader', str(probe), '-o', str(binary)],
                    check=True,
                )
                outputs[label] = subprocess.run(
                    [str(binary)], check=True, capture_output=True, text=True
                ).stdout.splitlines()
        self.assertEqual(['oled 1 1', 'erista 0 0', 'detected erista 0'], outputs['original'])
        self.assertEqual(['oled 1 1', 'erista 1 1', 'detected erista 1'], outputs['derived'])


class CommandLine(unittest.TestCase):
    def run_tool(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(TOOL), *arguments], capture_output=True, text=True, check=False)

    def test_derive_verify_and_agree_through_the_command_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out, stamp = Path(tmp, 'SwitchBoard.h'), Path(tmp, 'ok')
            flags = ['--early-trace', '--stop-after-vm', '--single-core', '--profile', 'debug']
            done = self.run_tool('derive', '--board', 'erista', '--header', str(HEADER_PATH), '--out', str(out), *flags)
            self.assertEqual(0, done.returncode, done.stderr)
            self.assertEqual(derive(HEADER, 'erista', **ALL), out.read_text())
            self.assertEqual(
                0,
                self.run_tool(
                    'verify', '--board', 'erista', '--header', str(HEADER_PATH), '--derived', str(out)
                ).returncode,
            )
            script = Path(tmp, 'script.txt')
            script.write_text(board_gate(out.read_text()))
            done = self.run_tool('agree', '--derived', str(out), '--script', str(script), '--stamp', str(stamp))
            self.assertEqual(0, done.returncode, done.stderr)
            self.assertTrue(stamp.exists())

    def test_every_missing_condition_is_a_refusal_with_no_output_file(self) -> None:
        full = ['--early-trace', '--stop-after-vm', '--single-core', '--profile', 'debug']
        for dropped in ('--early-trace', '--stop-after-vm', '--single-core'):
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp, 'SwitchBoard.h')
                flags = [f for f in full if f != dropped]
                done = self.run_tool(
                    'derive', '--board', 'erista', '--header', str(HEADER_PATH), '--out', str(out), *flags
                )
                self.assertNotEqual(0, done.returncode, dropped)
                self.assertRegex(done.stderr, 'refused')
                self.assertFalse(out.exists(), dropped)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp, 'SwitchBoard.h')
            flags = ['--early-trace', '--stop-after-vm', '--single-core', '--profile', 'release']
            done = self.run_tool('derive', '--board', 'erista', '--header', str(HEADER_PATH), '--out', str(out), *flags)
            self.assertNotEqual(0, done.returncode)
            self.assertFalse(out.exists())

    def test_no_boot_or_features_of_any_other_row_appears_in_the_output(self) -> None:
        derived = derive(HEADER, 'erista', **ALL)
        self.assertEqual(1, len(re.findall(r'X\(ERISTA, "erista", 0, 0, 1,', derived)))
        self.assertEqual(
            [r['boot'] for r in rows(HEADER)],
            [r['boot'] if r['name'] != 'erista' else '0' for r in rows(derived)],
        )


if __name__ == '__main__':
    unittest.main()
