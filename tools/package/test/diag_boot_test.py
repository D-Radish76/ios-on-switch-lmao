"""The read only diagnostic: what its script may run, what it may read, and that nothing in it can boot iPadOS.

The commands and the addresses are listed here on their own, not derived from the generator, so a change to either list
is a change to this file as well.
"""

import re
import tempfile
import unittest
from argparse import Namespace
from collections.abc import Callable
from pathlib import Path

from bl33 import ENV_EDITS, bl33_patch
from boot_script import boards
from diag_boot import (
    DWELL,
    IDENTITY_REGISTERS,
    build,
    carveout_table,
    compare,
    diag_bl33,
    diag_script,
    diag_u_boot,
    read_addresses,
)
from loader_abi import BL33_BASE, DTB_SLOT, EXPORTER_SLOT, LOADER_SLOT, SCRIPT_SLOT, STAGE_HEADER

HEADER = Path('kits/SwitchBoard/SwitchBoard.h').read_text()
LOADER = Path('boot/loader/loader.c').read_text()
TEMPLATE = Path('boot/diag/u-boot-diag-script.txt').read_text()
BL33 = Path('bazel/pinned/firmware/bl33.bin').read_bytes()
BL31 = Path('bazel/pinned/firmware/bl31.bin')

COMMANDS = {'setenv', 'setexpr', 'test', 'echo', 'md.l', 'sleep', 'true', 'reset'}
STRUCTURE = {'if', 'then', 'else', 'fi', 'while', 'do', 'done'}
READABLE = [0x70000804, 0x7000F9D8, *range(0x70019000, 0x70019E00, 4)]
MC_WINDOW = range(0x70019000, 0x70019E00)
LOG = """70000804: 00000210
70019050: 00001000
70019670: 00000001
"""


def commands(script: str) -> list[list[str]]:
    """Every simple command of the script as a token list, the if and while conditions included."""
    found = []
    for raw in script.splitlines():
        line = raw.strip()
        if not line or line.startswith(('#', 'for ')):
            continue
        for part in re.split(r';\s*|\s+then$|\s+do$', line):
            tokens = part.split()
            while tokens and tokens[0] in STRUCTURE:
                tokens = tokens[1:]
            if tokens:
                found.append(tokens)
    return found


class DiagScript(unittest.TestCase):
    script = diag_script(TEMPLATE, HEADER, LOADER)

    def test_only_allowed_commands(self):
        for tokens in commands(self.script):
            self.assertIn(tokens[0], COMMANDS, ' '.join(tokens))

    def test_nothing_that_writes_or_boots(self):
        used = {t[0] for t in commands(self.script)}
        for forbidden in ('mw', 'cp', 'cmp', 'load', 'source', 'run', 'bootm', 'booti', 'bootz', 'go', 'fatwrite'):
            self.assertNotIn(forbidden, used)
        for forbidden in ('fdt',):
            self.assertNotIn(forbidden, used)
        self.assertNotRegex(self.script, r'\$\(|`|\bbootm\b|LOADER|EXPORTER')

    def test_echo_text_has_no_separator(self):
        for line in self.script.splitlines():
            if line.strip().startswith('echo '):
                self.assertNotIn(';', line)

    def test_reads_are_one_literal_aligned_word_inside_the_allowed_windows(self):
        reads = [t for t in commands(self.script) if t[0] == 'md.l']
        self.assertEqual(len(reads), 2 + len(carveout_table(LOADER)))
        for _, address, count in reads:
            self.assertEqual(count, '1')
            self.assertRegex(address, r'^0x[0-9a-f]{8}$')
            value = int(address, 16)
            self.assertIn(value, READABLE)
            self.assertEqual(value % 4, 0)
            self.assertTrue(value in IDENTITY_REGISTERS or value in MC_WINDOW)

    def test_reads_are_native_gates_table_in_its_order(self):
        reads = [int(t[1], 16) for t in commands(self.script) if t[0] == 'md.l']
        self.assertEqual(reads, read_addresses(LOADER))
        self.assertEqual(len(carveout_table(LOADER)), 25)
        self.assertEqual(reads[:2], [0x70000804, 0x7000F9D8])

    def test_it_ends_in_a_reset_after_a_bounded_wait_and_then_a_loop(self):
        lines = [line.strip() for line in self.script.splitlines() if line.strip()]
        self.assertEqual(lines[-3:], ['while true; do', 'sleep 1', 'done'])

    def test_no_line_claims_where_the_reset_lands(self):
        for text in (self.script, ''.join(diag_u_boot(BL33).decode('latin1').split('\0'))):
            for sentence in re.findall(r'[^\n]*hekate[^\n]*', text):
                if 'echo' in sentence:
                    self.assertTrue('Expected' in sentence or 'expected' in sentence, sentence)
                    self.assertIn('not verified', sentence)

    def test_reset_is_used_once_and_only_after_the_wait(self):
        lines = [line.strip() for line in self.script.splitlines() if line.strip() and not line.startswith('#')]
        self.assertEqual(lines.count('reset'), 1)
        wait = lines.index('for left in 10 5 0; do')
        self.assertEqual(
            lines[wait : wait + 5],
            [
                'for left in 10 5 0; do',
                'sleep 5',
                'echo RESET in ${left} s. Expected to land in hekate, not verified',
                'done',
                'reset',
            ],
        )
        self.assertNotIn('reset', [t[0] for t in commands('\n'.join(lines[:wait]))])

    def test_every_table_row_is_let_through_and_none_is_enabled_by_it(self):
        rows = boards(HEADER)
        for name, t210b01, sku, _ in rows:
            self.assertIn(f'if test "${{t210b01}}" = "{t210b01}" -a "${{sku}}" = "{sku}"; then', self.script, name)
        self.assertEqual([boot for name, _, _, boot in rows if name == 'erista'], [False])

    def test_a_template_without_a_marker_stops_the_build(self):
        with self.assertRaises(SystemExit):
            diag_script(TEMPLATE.replace('@READS@', ''), HEADER, LOADER)


class Hush:
    """The few commands the script uses, run the way U-Boot's hush would, up to the script's final loop."""

    def __init__(self, variables: dict, md_l: Callable[[int], bool]) -> None:
        self.env, self.out, self.reads, self.md_l = dict(variables), [], [], md_l

    def expand(self, text: str) -> str:
        return re.sub(r'\$\{(\w+)\}', lambda m: self.env.get(m.group(1), ''), text)

    def test(self, rest: list[str]) -> bool:
        clauses, current = [], []
        for token in rest:
            if token == '-a':
                clauses.append(current)
                current = []
            else:
                current.append(token)
        return all(len(c) == 3 and c[1] == '=' and c[0] == c[2] for c in [*clauses, current])

    def simple(self, line: str) -> bool:
        name, *rest = self.expand(line).replace('"', '').split()
        if name == 'setenv':
            self.env[rest[0]] = ' '.join(rest[1:])
        elif name == 'setexpr':
            self.env[rest[0]] = f'{int(rest[1], 16) + int(rest[3], 16):x}'
        elif name == 'echo':
            self.out.append(' '.join(rest))
        elif name == 'md.l':
            self.reads.append(int(rest[0], 16))
            return self.md_l(int(rest[0], 16))
        elif name == 'test':
            return self.test(rest)
        else:
            raise AssertionError(line)
        return True

    def run(self, script: str) -> 'Hush':
        stack = []  # (condition now, condition as first tested)
        for raw in script.splitlines():
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith(('while', 'for ')):
                break
            running = all(now for now, _ in stack)
            if line.startswith('if '):
                condition = running and self.simple(line[3:].removesuffix('; then'))
                stack.append((condition, condition))
            elif line == 'else':
                stack[-1] = (not stack[-1][1], stack[-1][1])
            elif line == 'fi':
                stack.pop()
            elif running:
                self.simple(line)
        return self


def run_script(script: str, variables: dict, md_l: Callable[[int], bool] = lambda _: True) -> Hush:
    return Hush(variables, md_l).run(script)


ERISTA = {'t210b01': '0', 'sku': '0'}
TOTAL = 2 + 25


class DiagRun(unittest.TestCase):
    script = diag_script(TEMPLATE, HEADER, LOADER)

    def test_all_reads_ok_says_done_with_the_full_count(self):
        ran = run_script(self.script, ERISTA)
        self.assertEqual(len(ran.reads), TOTAL)
        self.assertEqual(ran.env['diag_reads'], f'{TOTAL:x}')
        self.assertIn(f'DONE: all {TOTAL} registers read', ran.out)

    def test_a_failed_read_stops_the_reads_and_never_says_done(self):
        failing = read_addresses(LOADER)[5]
        ran = run_script(self.script, ERISTA, lambda address: address != failing)
        self.assertEqual(ran.reads, read_addresses(LOADER)[:6])
        self.assertEqual(ran.env['diag_reads'], '5')
        self.assertEqual(ran.env['diag_status'], f'READ_FAILED_{failing:08x}')
        self.assertFalse(any(line.startswith('DONE') for line in ran.out))
        self.assertTrue(any(line.startswith('INCOMPLETE') for line in ran.out))

    def test_a_console_that_failed_reads_nothing(self):
        script = self.script.replace('if setenv stdout vidconsole; then', 'if test "1" = "2"; then')
        ran = run_script(script, ERISTA)
        self.assertEqual(ran.env['diag_console'], 'FAILED')
        self.assertEqual(ran.reads, [])
        self.assertTrue(any(line.startswith('REFUSED') for line in ran.out))

    def test_a_board_not_in_the_table_reads_nothing(self):
        ran = run_script(self.script, {'t210b01': '1', 'sku': '9'})
        self.assertEqual(ran.reads, [])
        self.assertTrue(any(line.startswith('REFUSED') for line in ran.out))

    def test_no_setexpr_reads_memory(self):
        for tokens in commands(self.script):
            if tokens[0] == 'setexpr':
                self.assertEqual(len(tokens), 5, tokens)
                self.assertEqual(tokens[0:2], ['setexpr', 'diag_reads'])
                self.assertFalse(any('*' in t for t in tokens))


class DiagFallback(unittest.TestCase):
    """What U-Boot does when the SD card, the script or hekate's environment is not as expected."""

    def entries(self, image: bytes) -> dict[str, str]:
        start = image.index(b'\0bootcmd=') + 1
        block = image[start : image.index(b'\0\0', start)]
        return {e.split(b'=', 1)[0].decode(): e.decode() for e in block.split(b'\0') if b'=' in e}

    def test_extlinux_is_never_run_and_the_failure_path_never_resets(self):
        env = self.entries(diag_u_boot(BL33))
        self.assertEqual(env['scan_dev_for_extlinux'], 'scan_dev_for_extlinux=true')
        self.assertEqual(env['distro_bootcmd'].count('reset'), 1)
        self.assertIn('sleep 15;reset;while true; do sleep 1; done;', env['distro_bootcmd'])
        self.assertNotIn('Rebooting in 10s', env['distro_bootcmd'])
        self.assertEqual(env['boot_targets'], 'boot_targets=mmc1')
        self.assertNotIn('bootcmd_mmc0', env)

    def test_the_script_slot_is_the_only_script_source(self):
        env = self.entries(diag_u_boot(BL33))
        self.assertEqual(
            env['scan_dev_for_scripts'],
            f'scan_dev_for_scripts=source {SCRIPT_SLOT[0]:#x}; echoe SCRIPT FAILED: continuing...',
        )
        self.assertNotIn('boot_a_script', env)

    def test_the_normal_image_is_not_changed_by_it(self):
        normal = bl33_patch(BL33)
        self.assertIn(b'run boot_extlinux', normal)
        self.assertIn(b'Rebooting in 10s', normal)
        self.assertNotEqual(normal, diag_u_boot(BL33))
        self.assertEqual(len(normal), len(diag_u_boot(BL33)))

    def test_an_edit_for_an_entry_that_is_not_there_stops_the_build(self):
        with self.assertRaises(SystemExit):
            bl33_patch(BL33, {b'nothing_like_this=1': b'x=1'})
        self.assertNotIn(b'nothing_like_this=1', ENV_EDITS)


class DiagWait(unittest.TestCase):
    def test_the_wait_is_one_number_in_the_script_and_in_u_boot(self):
        for dwell, steps in ((10, '5 0'), (15, '10 5 0'), (60, '55 50 45 40 35 30 25 20 15 10 5 0')):
            script = diag_script(TEMPLATE, HEADER, LOADER, dwell)
            self.assertIn(f'for left in {steps}; do', script)
            self.assertIn(f'Reset in {dwell} s, or hold', script)
            image = diag_u_boot(BL33, dwell)
            self.assertIn(f'Reset in {dwell} s, expected'.encode(), image)
            self.assertIn(f'sleep {dwell};reset;'.encode(), image)

    def test_a_wait_outside_what_a_photograph_needs_is_refused(self):
        for dwell in (0, 5, 12, 61, 120):
            with self.assertRaises(SystemExit):
                diag_script(TEMPLATE, HEADER, LOADER, dwell)
            with self.assertRaises(SystemExit):
                diag_u_boot(BL33, dwell)

    def test_the_default_is_short(self):
        self.assertLessEqual(DWELL, 20)


class DiagImage(unittest.TestCase):
    def test_u_boot_then_the_script_and_nothing_else(self):
        script = diag_script(TEMPLATE, HEADER, LOADER)
        image = diag_bl33(diag_u_boot(BL33), script)
        patched = diag_u_boot(BL33)
        slot = SCRIPT_SLOT[0] - BL33_BASE
        self.assertEqual(image[: len(patched)], patched)
        self.assertEqual(image[len(patched) : slot], bytes(slot - len(patched)))
        self.assertIn(script.encode('ascii'), image[slot:])
        self.assertLess(len(image), LOADER_SLOT[0] - BL33_BASE)
        for address in (LOADER_SLOT[0], EXPORTER_SLOT[0], DTB_SLOT[0], STAGE_HEADER):
            self.assertLessEqual(len(image), address - BL33_BASE)

    def test_u_boot_that_reaches_the_slot_is_refused(self):
        script = diag_script(TEMPLATE, HEADER, LOADER)
        with self.assertRaises(SystemExit):
            diag_bl33(bytes(SCRIPT_SLOT[0] - BL33_BASE + 1), script)


class DiagPackage(unittest.TestCase):
    def test_own_entry_and_nothing_of_the_normal_one(self):
        with tempfile.TemporaryDirectory() as out:
            build(
                Namespace(
                    template=Path('boot/diag/u-boot-diag-script.txt'),
                    boards=Path('kits/SwitchBoard/SwitchBoard.h'),
                    loader_src=Path('boot/loader/loader.c'),
                    bl31=BL31,
                    bl33=Path('bazel/pinned/firmware/bl33.bin'),
                    out=Path(out),
                    dwell=DWELL,
                )
            )
            files = sorted(str(p.relative_to(out)) for p in Path(out).rglob('*') if p.is_file())
            self.assertEqual(
                files,
                [
                    'bootloader/ini/ios-diag.ini',
                    'switchroot/ios-diag/bl31.bin',
                    'switchroot/ios-diag/bl33.bin',
                    'switchroot/ios-diag/script.txt',
                ],
            )
            ini = Path(out, 'bootloader/ini/ios-diag.ini').read_text()
            self.assertIn('boot_prefixes=/switchroot/ios-diag/', ini)
            self.assertNotIn('/switchroot/ios/', ini)
            self.assertIn('scan_dev_for_extlinux=true', ini)
            self.assertIn('bootcmd_mmc0=true', ini)
            self.assertNotIn('ram_oc', ini)
            self.assertEqual(Path(out, 'switchroot/ios-diag/bl31.bin').read_bytes(), BL31.read_bytes())


class Compare(unittest.TestCase):
    def test_what_was_read_against_the_oled_values(self):
        lines = compare(LOG, LOADER)
        self.assertEqual(lines[0], '0x70019050 00001000 oled 0x00001000 same')
        self.assertEqual(lines[1], '0x70019670 00000001 oled 0x00000000 DIFFERENT')
        self.assertTrue(lines[2].endswith('NOT READ'))
        self.assertEqual(len(lines), 25)


if __name__ == '__main__':
    unittest.main()
