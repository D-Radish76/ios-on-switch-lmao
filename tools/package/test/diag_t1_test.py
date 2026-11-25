"""T1 (boot/diag/loader/t1*.c): what its inspection requires, a real link through it, and what its sources contain."""

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
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T1_MAP = MAP + ' switch_t1_run switch_t1_access switch_t1_read32 switch_t1_write32 switch_t1_may_write'
READ32 = ('switch_diag_read32', 'ldr w0, [x0]', 'ret')


def function(name: str, *lines: str) -> str:
    body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
    return f'0000000000000100 <{name}>:\n{body}\n'


def image(write32: tuple[str, ...] = ('str w1, [x0]', 'ret'), caller: str = 'switch_t1_access') -> str:
    return (
        function(*READ32)
        + function('switch_t1_read32', 'ldr w0, [x0]', 'ret')
        + function('switch_t1_write32', *write32)
        + function(caller, 'bl 0x100 <switch_t1_write32>', 'b 0x100 <switch_t1_read32>')
        + (function('switch_t1_access', 'bl 0x100 <switch_t1_write32>') if caller != 'switch_t1_access' else '')
    )


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_t1_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, t1=True))
        self.assertEqual([], inspect_map(T1_MAP, t1=True))
        self.assertTrue(inspect_map(T1_MAP))

    def test_the_two_raw_accesses_must_be_one_instruction_each(self) -> None:
        self.assertEqual([], [p for p in inspect_disassembly(image(), t1=True) if 'switch_t1' in p])
        two = inspect_disassembly(image(('str w1, [x0]', 'str w1, [x0, #4]', 'ret')), t1=True)
        self.assertTrue([p for p in two if 'switch_t1_write32 is not' in p])
        moved = inspect_disassembly(image(('mov w8, w0', 'str w1, [x8]', 'ret')), t1=True)
        self.assertEqual([], [p for p in moved if 'switch_t1_write32 is not' in p])
        wrong = inspect_disassembly(image(('mov w8, w0', 'str w1, [x9]', 'ret')), t1=True)
        self.assertTrue([p for p in wrong if 'switch_t1_write32 is not' in p])

    def test_only_the_gate_may_call_a_raw_access(self) -> None:
        rogue = inspect_disassembly(image(caller='rogue'), t1=True)
        self.assertTrue([p for p in rogue if p.startswith('rogue: calls switch_t1_write32 outside')])
        self.assertTrue([p for p in rogue if p.startswith('rogue: calls switch_t1_read32 outside')])

    def test_a_tail_call_to_the_raw_access_is_a_call(self) -> None:
        text = image() + function('sneaky', 'b 0x100 <switch_t1_write32>')
        self.assertTrue([p for p in inspect_disassembly(text, t1=True) if p.startswith('sneaky')])


class Sources(unittest.TestCase):
    def test_every_register_address_is_in_car_pinmux_or_i2c1(self) -> None:
        text = (LOADER / 't1_regs.c').read_text()
        bases = dict(re.findall(r'#define (CAR|MUX|I2C1)\s+(0x[0-9a-f]+)u', text))
        self.assertEqual({'CAR': '0x60006000', 'MUX': '0x70003000', 'I2C1': '0x7000c000'}, bases)
        offsets = [int(o, 16) for _, o in re.findall(r'\{(CAR|MUX|I2C1) \+ (0x[0-9a-f]+),', text)]
        self.assertEqual(13, len(offsets))
        self.assertLess(max(offsets), 0x200)

    def test_only_the_allowlist_file_names_a_device_register_base(self) -> None:
        for name in ('t1.c', 't1_gate.c', 't1_report.c', 't1_access.c', 'diag.c'):
            text = (LOADER / name).read_text()
            for base in ('0x60006', '0x7000c', '0x70003', '0x7000a', '0x6000d', '0x7000e'):
                self.assertNotIn(base, text.lower(), f'{name} names {base}')

    def test_the_sources_name_no_peripheral_the_stage_may_not_touch(self) -> None:
        for name in ('t1.c', 't1.h', 't1_regs.c', 't1_gate.c', 't1_access.c'):
            text = (LOADER / name).read_text().lower()
            for word in ('pwm_csr', 'regulator', 'gpio_out', 'sd_xfer', 'pmc_', 'max77'):
                self.assertNotIn(word, text, f'{name} mentions {word}')

    def test_the_raw_accesses_are_one_load_and_one_store(self) -> None:
        sites = [
            line.strip()
            for line in (LOADER / 't1_access.c').read_text().splitlines()
            if '(uintptr_t)' in line and 'volatile' in line
        ]
        self.assertEqual(
            [
                'return *(const volatile uint32_t *)(uintptr_t)address;',
                '*(volatile uint32_t *)(uintptr_t)address = value;',
            ],
            sites,
        )

    def test_no_em_dash_in_the_new_files(self) -> None:
        for name in ('t1.c', 't1.h', 't1_regs.c', 't1_gate.c', 't1_report.c', 't1_report.h', 't1_access.c'):
            self.assertNotIn('—', (LOADER / name).read_text())


class Package(unittest.TestCase):
    def test_the_entry_is_its_own_and_boots_nothing(self) -> None:
        ini = entry_ini('iOS Diag T1', 'ios-diag-t1', 'iDiagT1')
        self.assertIn('[iOS Diag T1]', ini)
        self.assertIn('boot_prefixes=/switchroot/ios-diag-t1/', ini)
        self.assertIn('id=iDiagT1', ini)
        for other in ('iDiagL\n', 'iDiagLR', 'iDiagLS', 'ios-diag-loader/'):
            self.assertNotIn(other, ini)


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump'), 'no LLVM on PATH'
)
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [
        '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
        '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-O2', '-Wall', '-Wextra', '-Werror', '-DPROBE_QUIET=1',
        '-DSWITCH_DIAG_T1=1',
    ]  # fmt: skip
    INCLUDES: ClassVar[list[str]] = [*('boot/diag/loader', 'boot/handoff', 'boot/loader', 'kits/SwitchBoard')]
    SOURCES: ClassVar[list[str]] = [
        *(f'boot/diag/loader/{n}' for n in ('entry.S', 'diag.c', 'checks.c', 'report.c')),
        *(f'boot/diag/loader/{n}' for n in ('t1.c', 't1_regs.c', 't1_gate.c', 't1_report.c', 't1_access.c')),
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
        elf, linkmap = f'{scratch}/t1.elf', f'{scratch}/t1.map'
        linked = subprocess.run(
            ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        self.assertEqual(0, linked.returncode, linked.stderr)
        dis = subprocess.run(
            ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
        ).stdout
        return Path(linkmap).read_text(), dis

    def test_it_links_and_passes_the_t1_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            text, dis = self.link(scratch, [])
            self.assertEqual([], inspect_map(text, t1=True))
            self.assertEqual([], inspect_disassembly(dis, t1=True))
            self.assertTrue(inspect_map(text))

    def test_a_second_user_of_a_raw_access_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nvoid switch_t1_write32(uint32_t, uint32_t);\n'
                'void rogue(void) { switch_t1_write32(0x7000c000u, 0); }\n'
            )
            _, dis = self.link(scratch, [str(rogue)])
            self.assertTrue([p for p in inspect_disassembly(dis, t1=True) if p.startswith('rogue: calls')])


if __name__ == '__main__':
    unittest.main()
