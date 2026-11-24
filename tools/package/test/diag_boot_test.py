"""The read only diagnostic: what its script may run, what it may read, and that nothing in it can boot iPadOS.

The commands and the addresses are listed here on their own, not derived from the generator, so a change to either list
is a change to this file as well.
"""

import re
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

from bl33 import bl33_patch
from boot_script import boards
from diag_boot import (
    IDENTITY_REGISTERS,
    build,
    carveout_table,
    compare,
    diag_bl33,
    diag_script,
    read_addresses,
)
from loader_abi import BL33_BASE, DTB_SLOT, EXPORTER_SLOT, LOADER_SLOT, SCRIPT_SLOT, STAGE_HEADER

HEADER = Path('kits/SwitchBoard/SwitchBoard.h').read_text()
LOADER = Path('boot/loader/loader.c').read_text()
TEMPLATE = Path('boot/diag/u-boot-diag-script.txt').read_text()
BL33 = Path('bazel/pinned/firmware/bl33.bin').read_bytes()
BL31 = Path('bazel/pinned/firmware/bl31.bin')

COMMANDS = {'setenv', 'test', 'echo', 'md.l', 'sleep', 'true'}
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
        if not line or line.startswith('#'):
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
        for forbidden in ('reset', 'fdt'):
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

    def test_it_ends_in_a_loop_it_never_leaves(self):
        tail = [line.strip() for line in self.script.splitlines() if line.strip()][-3:]
        self.assertEqual(tail, ['while true; do', 'sleep 1', 'done'])

    def test_every_table_row_is_let_through_and_none_is_enabled_by_it(self):
        rows = boards(HEADER)
        for name, t210b01, sku, _ in rows:
            self.assertIn(f'if test "${{t210b01}}" = "{t210b01}" -a "${{sku}}" = "{sku}"; then', self.script, name)
        self.assertEqual([boot for name, _, _, boot in rows if name == 'erista'], [False])

    def test_a_template_without_a_marker_stops_the_build(self):
        with self.assertRaises(SystemExit):
            diag_script(TEMPLATE.replace('@READS@', ''), HEADER, LOADER)


class DiagImage(unittest.TestCase):
    def test_u_boot_then_the_script_and_nothing_else(self):
        script = diag_script(TEMPLATE, HEADER, LOADER)
        image = diag_bl33(bl33_patch(BL33), script)
        patched = bl33_patch(BL33)
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
