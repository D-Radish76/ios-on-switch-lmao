"""T1-R (boot/diag/loader/t1r*.c, its U-Boot script): the inspection, a real link, the script and the sources."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from diag_loader import entry_ini
from diag_loader_inspect import inspect_disassembly, inspect_map

LOADER = Path('boot/diag/loader')
SCRIPT = LOADER / 'u-boot-script-t1r.txt'
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T1R_MAP = MAP + ' switch_t1r_run switch_t1r_access switch_t1r_read32 switch_t1r_may_read switch_t1r_address'
READ32 = ('switch_diag_read32', 'ldr w0, [x0]', 'ret')
CAR_READS = ('0x60006124', '0x60006128', '0x60006004', '0x60006010', '0x600060a0')
COMMANDS = {
    'setenv', 'if', 'else', 'fi', 'then', 'echoe', 'base', 'test', 'fdt', 'itest.l', 'cp.b', 'bootm', 'md.l', 'sleep',
    'for', 'done', 'while', 'reset',
}  # fmt: skip
FORBIDDEN = ('mw', 'mm', 'nm', 'mw.l', 'mw.b', 'i2c', 'saveenv', 'fatload', 'fatwrite', 'sf', 'mmc', 'gpio', 'pmic')


def function(name: str, *lines: str) -> str:
    body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
    return f'0000000000000100 <{name}>:\n{body}\n'


def image(
    address_store: str = 'ret', caller: str = 'switch_t1r_access', raw: tuple[str, ...] = ('ldr w0, [x0]', 'ret')
) -> str:
    return (
        function(*READ32)
        + function('switch_t1r_read32', *raw)
        + function('switch_t1r_address', address_store, 'ret')
        + function('switch_t1r_may_read', 'cmp w0, #16', 'ret')
        + function('switch_t1r_hekate_source', 'ret')
        + function(caller, 'bl 0x100 <switch_t1r_address>', 'b 0x100 <switch_t1r_read32>')
        + (function('switch_t1r_access', 'bl 0x100 <switch_t1r_address>') if caller != 'switch_t1r_access' else '')
    )


def code(text: str) -> str:
    return '\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))


def commands(text: str) -> list[str]:
    found = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(('#', '@')):
            continue
        line = re.sub(r'^(if|elif)\s+', '', line)
        found.append(line.split()[0])
    return found


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_t1r_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, t1r=True))
        self.assertEqual([], inspect_map(T1R_MAP, t1r=True))
        self.assertTrue(inspect_map(T1R_MAP))

    def test_a_t1_write_symbol_in_a_t1r_link_is_refused(self) -> None:
        self.assertTrue(inspect_map(T1R_MAP + ' switch_t1_write32', t1r=True))
        self.assertTrue(inspect_map(T1R_MAP + ' switch_t1r_write32', t1r=True))

    def test_the_raw_read_is_one_load_and_a_return(self) -> None:
        self.assertEqual([], [p for p in inspect_disassembly(image(), t1r=True) if 'switch_t1r' in p])
        two = inspect_disassembly(image(raw=('ldr w0, [x0]', 'ldr w1, [x0, #4]', 'ret')), t1r=True)
        self.assertTrue([p for p in two if 'switch_t1r_read32 is not' in p])

    def test_only_the_gate_may_call_the_raw_read(self) -> None:
        rogue = inspect_disassembly(image(caller='rogue'), t1r=True)
        self.assertTrue([p for p in rogue if p.startswith('rogue: calls switch_t1r_read32 outside')])

    def test_a_tail_call_to_the_raw_read_is_a_call(self) -> None:
        text = image() + function('sneaky', 'b 0x100 <switch_t1r_read32>')
        self.assertTrue([p for p in inspect_disassembly(text, t1r=True) if p.startswith('sneaky')])

    def test_a_store_in_the_address_path_is_refused(self) -> None:
        stored = inspect_disassembly(image(address_store='str w1, [x0]'), t1r=True)
        self.assertTrue([p for p in stored if p.startswith('switch_t1r_address')])
        saved = inspect_disassembly(image(address_store='ret'), t1r=True)
        self.assertEqual([], [p for p in saved if p.startswith('switch_t1r_address')])


class Script(unittest.TestCase):
    text = code(SCRIPT.read_text())

    def test_only_listed_commands_appear(self) -> None:
        self.assertEqual(set(), {c for c in commands(self.text) if c not in COMMANDS})
        for word in commands(self.text):
            self.assertNotIn(word, FORBIDDEN)

    def test_it_adds_only_md_to_the_d0_commands(self) -> None:
        d0 = set(commands(code((LOADER / 'u-boot-script.txt').read_text())))
        self.assertEqual({'md.l'}, set(commands(self.text)) - d0)

    def test_no_forbidden_command_appears_anywhere_in_the_text(self) -> None:
        for word in FORBIDDEN:
            self.assertIsNone(re.search(rf'(^|[;&|]\s*){re.escape(word)}\s', self.text, re.MULTILINE), word)

    def test_md_only_reads_the_five_car_words_and_each_point_reads_all_five(self) -> None:
        reads = re.findall(r'^\s*md\.l\s+(\S+)\s+(\S+)\s*$', self.text, re.MULTILINE)
        self.assertEqual(10, len(reads))
        self.assertEqual({'1'}, {count for _, count in reads})
        self.assertEqual(list(CAR_READS) * 2, [address for address, _ in reads])
        self.assertEqual(10, len(re.findall(r'\bmd\.l\b', self.text)))

    def test_the_points_are_labelled_and_ordered_around_bootm(self) -> None:
        order = [
            self.text.index(s) for s in ('T1R P1.', 'bootm start', 'bootm loados', 'bootm prep', 'T1R P2.', 'bootm go')
        ]
        self.assertEqual(sorted(order), order)
        self.assertIn('sleep 5', self.text)
        self.assertLess(self.text.index('T1R P2.'), self.text.index('sleep 5'))
        self.assertLess(self.text.index('sleep 5'), self.text.index('bootm go'))

    def test_the_first_md_comes_after_the_board_and_framebuffer_checks(self) -> None:
        first = self.text.index('md.l')
        self.assertLess(self.text.index('BOARD OK'), first)
        self.assertLess(self.text.index('FRAMEBUFFER OK'), first)

    def test_no_device_write_command_and_no_em_dash(self) -> None:
        self.assertNotIn('—', self.text)
        self.assertEqual([], re.findall(r'^\s*(?:mw|mm|nm)\b', self.text, re.MULTILINE))


class Sources(unittest.TestCase):
    FILES = ('t1r.c', 't1r.h', 't1r_regs.c', 't1r_gate.c', 't1r_access.c', 't1r_report.c', 't1r_report.h')

    def test_every_register_address_is_in_car_or_i2c1(self) -> None:
        text = (LOADER / 't1r_regs.c').read_text()
        bases = dict(re.findall(r'#define (CAR|I2C1)\s+(0x[0-9a-f]+)u', text))
        self.assertEqual({'CAR': '0x60006000', 'I2C1': '0x7000c000'}, bases)
        offsets = sorted(int(o, 16) for _, o in re.findall(r'\] = (CAR|I2C1) \+ (0x[0-9a-f]+)', text))
        self.assertEqual(16, len(offsets))
        self.assertLess(max(offsets), 0x700)

    def test_the_excluded_registers_are_not_listed(self) -> None:
        text = (LOADER / 't1r_regs.c').read_text().lower()
        for word in ('interface_timing', 'cmd_data', 'bus_clear', 'fifo', '0x7000c0'):
            if word != '0x7000c0':
                self.assertNotIn(word, text)

    def test_only_the_allowlist_file_names_a_device_register_base(self) -> None:
        for name in ('t1r.c', 't1r_gate.c', 't1r_report.c', 't1r_access.c', 'diag.c'):
            text = (LOADER / name).read_text().lower()
            for base in ('0x60006', '0x7000c', '0x70003', '0x7000a', '0x6000d', '0x7000e'):
                self.assertNotIn(base, text, f'{name} names {base}')

    def test_there_is_no_store_function_and_no_other_peripheral(self) -> None:
        for name in self.FILES:
            text = (LOADER / name).read_text().lower()
            for word in ('write32', 't1r_write', 'pwm_csr', 'regulator', 'gpio_out', 'pmc_', 'max77', 'i2c_send'):
                self.assertNotIn(word, text, f'{name} mentions {word}')

    def test_the_raw_access_is_one_volatile_load(self) -> None:
        sites = [
            line.strip()
            for line in (LOADER / 't1r_access.c').read_text().splitlines()
            if '(uintptr_t)' in line and 'volatile' in line
        ]
        self.assertEqual(['return *(const volatile uint32_t *)(uintptr_t)address;'], sites)

    def test_no_em_dash_in_the_new_files(self) -> None:
        for name in self.FILES:
            self.assertNotIn('—', (LOADER / name).read_text())


class Package(unittest.TestCase):
    def test_the_entry_is_its_own_and_boots_nothing(self) -> None:
        ini = entry_ini('iOS Diag T1R', 'ios-diag-t1r', 'iDiagT1R')
        self.assertIn('[iOS Diag T1R]', ini)
        self.assertIn('boot_prefixes=/switchroot/ios-diag-t1r/', ini)
        self.assertIn('id=iDiagT1R', ini)
        for other in ('iDiagT1\n', 'ios-diag-t1/', 'iDiagL\n', 'iDiagLR', 'iDiagLS', 'ios-diag-loader/'):
            self.assertNotIn(other, ini)

    def test_boot_stays_zero(self) -> None:
        self.assertNotIn('boot=1', entry_ini('iOS Diag T1R', 'ios-diag-t1r', 'iDiagT1R'))


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump'), 'no LLVM on PATH'
)
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [
        '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
        '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-O2', '-Wall', '-Wextra', '-Werror', '-DPROBE_QUIET=1',
        '-DSWITCH_DIAG_T1R=1',
    ]  # fmt: skip
    INCLUDES: ClassVar[list[str]] = [*('boot/diag/loader', 'boot/handoff', 'boot/loader', 'kits/SwitchBoard')]
    SOURCES: ClassVar[list[str]] = [
        *(f'boot/diag/loader/{n}' for n in ('entry.S', 'diag.c', 'checks.c', 'report.c')),
        *(f'boot/diag/loader/{n}' for n in ('t1r.c', 't1r_regs.c', 't1r_gate.c', 't1r_report.c', 't1r_access.c')),
        'boot/diag/loader/exit.c',
        'boot/loader/memory.c',
    ]

    def compile(self, scratch: str, source: str, index: int) -> str:
        obj = f'{scratch}/{index}.o'
        command = ['clang', *self.FLAGS, *(f for inc in self.INCLUDES for f in ('-I', inc)), '-c', source, '-o', obj]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode and 'unable to create target' in result.stderr:
            self.skipTest('this clang does not target AArch64')
        self.assertEqual(0, result.returncode, result.stderr)
        return obj

    def link(self, scratch: str, extra: list[str]) -> tuple[str, str]:
        objects = [self.compile(scratch, s, i) for i, s in enumerate([*self.SOURCES, *extra])]
        elf, linkmap = f'{scratch}/t1r.elf', f'{scratch}/t1r.map'
        linked = subprocess.run(
            ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        self.assertEqual(0, linked.returncode, linked.stderr)
        dis = subprocess.run(
            ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
        ).stdout
        return Path(linkmap).read_text(), dis

    def test_it_links_and_passes_the_t1r_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            text, dis = self.link(scratch, [])
            self.assertEqual([], inspect_map(text, t1r=True))
            self.assertEqual([], inspect_disassembly(dis, t1r=True))
            self.assertTrue(inspect_map(text))

    def test_a_second_user_of_the_raw_read_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nuint32_t switch_t1r_read32(uint32_t);\n'
                'uint32_t rogue(void) { return switch_t1r_read32(0x7000c000u); }\n'
            )
            _, dis = self.link(scratch, [str(rogue)])
            self.assertTrue([p for p in inspect_disassembly(dis, t1r=True) if p.startswith('rogue: calls')])


if __name__ == '__main__':
    unittest.main()
