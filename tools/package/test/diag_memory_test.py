"""The memory diagnostic: what its scripts may run, what its image holds, and what bdinfo does in the pinned binary.

The command lists are written here on their own, so a change to a script's commands is a change to this file too.
The disassembly audit needs an AArch64 capable llvm-objdump on PATH and is skipped without one.
"""

import shutil
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

from bdinfo_audit import bdinfo_function, calls, disassemble, parse, violations, wrap_elf
from bl33 import bl33_patch
from diag_boot import DWELL, diag_bl33, diag_env_edits, diag_script, diag_u_boot
from diag_memory import PAGES, build, ini, memory_script
from script_commands import FORBIDDEN, commands

ROOT = Path('.')
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
LOADER_C = (ROOT / 'boot/loader/loader.c').read_text()
FRAME = (ROOT / 'boot/diag/memory/u-boot-script.txt').read_text()
BODIES = {
    1: (ROOT / 'boot/diag/memory/page-1-bdinfo.txt').read_text(),
    2: (ROOT / 'boot/diag/memory/page-2-fdt.txt').read_text(),
}
BL33 = (ROOT / 'bazel/pinned/firmware/bl33.bin').read_bytes()
COMMANDS = {'setenv', 'test', 'echo', 'bdinfo', 'fdt', 'sleep', 'reset', 'true'}
FDT_FORMS = [
    ['fdt', 'addr', '${fdtcontroladdr}'],
    ['fdt', 'print', '/memory'],
    ['fdt', 'print', '/reserved-memory'],
    ['fdt', 'rsvmem', 'print'],
]
TAIL_LINES = 9


def script(page: int) -> str:
    return memory_script(FRAME, BODIES[page], HEADER, page)


class Scripts(unittest.TestCase):
    def test_only_the_listed_commands_run(self) -> None:
        for page in (1, 2):
            for tokens in commands(script(page)):
                self.assertIn(tokens[0], COMMANDS, ' '.join(tokens))

    def test_fdt_is_used_in_four_forms_and_page_one_has_none(self) -> None:
        self.assertEqual([t for t in commands(script(2)) if t[0] == 'fdt'], FDT_FORMS)
        self.assertEqual([t for t in commands(script(1)) if t[0] == 'fdt'], [])

    def test_fdt_addr_never_takes_a_size(self) -> None:
        for page in (1, 2):
            for tokens in commands(script(page)):
                if tokens[:2] == ['fdt', 'addr']:
                    self.assertEqual(len(tokens), 3)

    def test_bdinfo_is_on_page_one_alone(self) -> None:
        self.assertEqual([t for t in commands(script(1)) if t[0] == 'bdinfo'], [['bdinfo']])
        self.assertEqual([t for t in commands(script(2)) if t[0] == 'bdinfo'], [])

    def test_nothing_writes_loads_boots_or_saves(self) -> None:
        for page in (1, 2):
            used = {t[0] for t in commands(script(page))}
            for forbidden in FORBIDDEN:
                self.assertNotIn(forbidden, used)
            self.assertNotRegex(script(page), r'\$\(|`')
            code = '\n'.join(line for line in script(page).splitlines() if not line.lstrip().startswith(('#', 'echo')))
            self.assertNotRegex(code, r'loados|bootm|saveenv')

    def test_no_address_literal_appears_in_either_script(self) -> None:
        for page in (1, 2):
            self.assertNotRegex(script(page), r'0x[0-9a-fA-F]+')

    def test_echo_text_has_no_separator(self) -> None:
        for page in (1, 2):
            for line in script(page).splitlines():
                if line.strip().startswith('echo '):
                    self.assertNotIn(';', line)

    def test_the_wait_and_reset_are_the_first_diagnostics_own(self) -> None:
        template = (ROOT / 'boot/diag/u-boot-diag-script.txt').read_text()
        tail = diag_script(template, HEADER, LOADER_C).splitlines()[-TAIL_LINES:]
        for page in (1, 2):
            self.assertEqual(script(page).splitlines()[-TAIL_LINES:], tail)
            self.assertEqual([t for t in commands(script(page)) if t[0] == 'reset'], [['reset']])

    def test_the_board_gate_is_the_first_diagnostics(self) -> None:
        self.assertIn('setenv diag_status READY', script(1))
        self.assertIn('"${t210b01}" = "0" -a "${sku}" = "0"', script(1))

    def test_no_placeholder_is_left_and_the_page_numbers_differ(self) -> None:
        for page in (1, 2):
            self.assertNotRegex(script(page), r'@[A-Z_0-9]+@')
            self.assertIn(f'PAGE {page} OF 2', script(page))

    def test_each_page_fits_the_screen(self) -> None:
        for page in (1, 2):
            echoes = [line for line in script(page).splitlines() if line.strip().startswith('echo ')]
            self.assertLessEqual(len(echoes), 14)


class Package(unittest.TestCase):
    def test_each_image_is_u_boot_plus_one_script_and_nothing_else(self) -> None:
        u_boot = diag_u_boot(BL33, DWELL)
        self.assertEqual(u_boot, bl33_patch(BL33, diag_env_edits(DWELL)))
        for page in (1, 2):
            image = diag_bl33(u_boot, script(page))
            self.assertTrue(image.startswith(u_boot))
            self.assertLess(len(image), 0x100000)

    def test_the_entries_are_apart_from_every_other(self) -> None:
        self.assertEqual(len({p.entry for p in PAGES} | {'ios', 'ios-diag', 'ios-diag-loader'}), 5)
        self.assertEqual(len({p.ident for p in PAGES} | {'iDiag', 'iDiagL'}), 4)
        for page in PAGES:
            text = ini(page)
            self.assertIn(f'boot_prefixes=/switchroot/{page.entry}/', text)
            self.assertIn('bootwait=0', text)
            self.assertNotIn('ram_oc', text)
            self.assertNotIn('/switchroot/ios/', text)

    def test_build_writes_each_entry_and_nothing_else(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            source = Path(scratch) / 'in'
            source.mkdir()
            names = {'bl31.bin': b'bl31', 'bl33.bin': BL33}
            texts = {'boards.h': HEADER, 'frame': FRAME, 'page1': BODIES[1], 'page2': BODIES[2]}
            for name, data in names.items():
                (source / name).write_bytes(data)
            for name, text in texts.items():
                (source / name).write_text(text)
            out = Path(scratch) / 'out'
            build(
                Namespace(
                    frame=source / 'frame',
                    page1=source / 'page1',
                    page2=source / 'page2',
                    boards=source / 'boards.h',
                    bl31=source / 'bl31.bin',
                    bl33=source / 'bl33.bin',
                    out=out,
                    dwell=DWELL,
                )
            )
            written = sorted(str(p.relative_to(out)) for p in out.rglob('*') if p.is_file())
        self.assertEqual(
            written,
            [
                'bootloader/ini/ios-diag-memory-fdt.ini',
                'bootloader/ini/ios-diag-memory.ini',
                'switchroot/ios-diag-memory-fdt/bl31.bin',
                'switchroot/ios-diag-memory-fdt/bl33.bin',
                'switchroot/ios-diag-memory-fdt/script.txt',
                'switchroot/ios-diag-memory/bl31.bin',
                'switchroot/ios-diag-memory/bl33.bin',
                'switchroot/ios-diag-memory/script.txt',
            ],
        )


class Bdinfo(unittest.TestCase):
    def test_the_pinned_u_boot_has_the_fields_the_page_asks_for_and_no_lmb_dump(self) -> None:
        labels = (b'DRAM bank', b'-> start', b'-> size', b'relocaddr', b'reloc off', b'irq_sp', b'sp start ')
        for label in (*labels, b'TLB addr', b'fdt_blob'):
            self.assertIn(label, BL33, label)
        for absent in (b'lmb_dump', b'lmb dump', b'memory.cnt'):
            self.assertNotIn(absent, BL33)

    def test_the_audit_rejects_a_store_and_a_system_instruction(self) -> None:
        listing = (
            '   10: a9bc7bfd \tstp\tx29, x30, [sp, #-0x40]!\n'
            '   14: f9000000 \tstr\tx1, [x0]\n'
            '   18: d4000001 \tsvc\t#0x0\n'
            '   1c: d65f03c0 \tret\n'
        )
        self.assertEqual(len(violations(parse(listing))), 2)

    def test_the_audit_accepts_stack_stores(self) -> None:
        listing = (
            '   10: a9bc7bfd \tstp\tx29, x30, [sp, #-0x40]!\n'
            '   14: f9001bf7 \tstr\tx23, [sp, #0x30]\n'
            '   18: d65f03c0 \tret\n'
        )
        self.assertEqual(violations(parse(listing)), [])

    def test_the_elf_wrapper_is_well_formed(self) -> None:
        elf = wrap_elf(b'\x1f\x20\x03\xd5' * 4)
        self.assertEqual(elf[:4], b'\x7fELF')
        self.assertEqual(len(elf) % 8, 0)

    @unittest.skipUnless(shutil.which('llvm-objdump') or shutil.which('objdump'), 'no disassembler on PATH')
    def test_bdinfo_in_the_pinned_binary_only_reads_and_prints(self) -> None:
        objdump = shutil.which('llvm-objdump') or 'objdump'
        try:
            instructions = parse(disassemble(BL33, objdump))
        except RuntimeError as problem:
            self.skipTest(f'{objdump} cannot read AArch64: {problem}')
        if not instructions:
            self.skipTest(f'{objdump} printed no AArch64 instructions')
        try:
            function = bdinfo_function(BL33, instructions)
        except ValueError as problem:
            self.skipTest(f'the listing of {objdump} is not in llvm-objdump format: {problem}')
        self.assertEqual(violations(function), [])
        self.assertLess(len(function), 100)
        self.assertEqual(len(calls(function)), 2)


if __name__ == '__main__':
    unittest.main()
