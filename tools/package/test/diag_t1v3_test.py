"""T1 v3: the inspection, a real link, the device addresses its sources may hold, the entry and the other images.

The other images must be what boot/diag/erista-diag-image-hashes.txt records.
"""

import contextlib
import io
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

import diag_images
from diag_loader import entry_ini
from diag_loader_inspect import inspect_disassembly, inspect_map

LOADER = Path('boot/diag/loader')
HASHES = Path('boot/diag/erista-diag-image-hashes.txt')
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T1V3_MAP = MAP + ' switch_t1v3_run switch_t1v3_access switch_t1v3_read32 switch_t1v3_write32 switch_t1v3_may_write'
READ32 = ('switch_diag_read32', 'ldr w0, [x0]', 'ret')
STORE32 = ('mov w8, w0', 'str w1, [x8]', 'ret')
DEVICE_LITERAL = re.compile(r'0x[5-7][0-9a-f]{7}u?\b', re.IGNORECASE)
SOURCES = ('t1v3.c', 't1v3.h', 't1v3_regs.c', 't1v3_gate.c', 't1v3_access.c', 't1v3_report.c', 't1v3_report.h')
ALLOWED_OFFSETS = {
    'CAR': (0x004, 0x010, 0x124),
    'I2C1': (0x00, 0x04, 0x0C, 0x1C, 0x68, 0x6C, 0x8C, 0x94, 0x98),
}
ALLOWED_WRITES = {
    'T1V3_I2C1_CMD_ADDR0': ('0x98u', '0x99u'),
    'T1V3_I2C1_CMD_DATA1': ('SWITCH_T1V3_ID_REGISTER',),
    'T1V3_I2C1_CNFG': ('0x2800u', '0x2840u', '0x2a00u', '0x2a40u'),
    'T1V3_I2C1_CLK_DIVISOR': ('SWITCH_T1V3_DIVISOR_AFTER',),
    'T1V3_I2C1_CONFIG_LOAD': ('SWITCH_T1V3_LOAD_APPLY', 'SWITCH_T1V3_LOAD_TRANSFER'),
}


def function(name: str, *lines: str) -> str:
    body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
    return f'0000000000000100 <{name}>:\n{body}\n'


def image(caller: str = 'switch_t1v3_access', raw_read: tuple[str, ...] = ('ldr w0, [x0]', 'ret')) -> str:
    return (
        function(*READ32)
        + function('switch_t1v3_read32', *raw_read)
        + function('switch_t1v3_write32', *STORE32)
        + function(caller, 'bl 0x100 <switch_t1v3_read32>', 'b 0x100 <switch_t1v3_write32>')
        + (function('switch_t1v3_access', 'bl 0x100 <switch_t1v3_read32>') if caller != 'switch_t1v3_access' else '')
    )


def problems_of(text: str) -> list[str]:
    return [p for p in inspect_disassembly(text, t1v3=True) if 'switch_t1v3' in p or p.startswith('rogue')]


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_t1v3_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, t1v3=True))
        self.assertEqual([], inspect_map(T1V3_MAP, t1v3=True))
        self.assertTrue(inspect_map(T1V3_MAP))

    def test_a_t1_or_t1r_symbol_in_a_t1v3_link_is_refused(self) -> None:
        self.assertTrue(inspect_map(T1V3_MAP + ' switch_t1_run', t1v3=True))
        self.assertTrue(inspect_map(T1V3_MAP + ' switch_t1r_read32', t1v3=True))

    def test_t1v3_symbols_are_not_taken_for_t1_ones(self) -> None:
        self.assertEqual([], [p for p in inspect_map(T1V3_MAP, t1v3=True) if 'T1' in p and 'T1 v3' not in p])

    def test_each_raw_access_is_one_instruction_and_a_return(self) -> None:
        self.assertEqual([], problems_of(image()))
        two = problems_of(image(raw_read=('ldr w0, [x0]', 'ldr w1, [x0, #4]', 'ret')))
        self.assertTrue([p for p in two if 'switch_t1v3_read32 is not' in p])

    def test_only_the_gate_may_call_a_raw_access(self) -> None:
        self.assertTrue([p for p in problems_of(image(caller='rogue')) if p.startswith('rogue: calls')])

    def test_a_tail_call_is_a_call(self) -> None:
        text = image() + function('sneaky', 'b 0x100 <switch_t1v3_write32>')
        self.assertTrue([p for p in problems_of(text) if p.startswith('sneaky')])


class Sources(unittest.TestCase):
    def test_every_register_address_is_in_car_or_i2c1_and_is_a_reviewed_one(self) -> None:
        text = (LOADER / 't1v3_regs.c').read_text()
        self.assertEqual(
            {'CAR': '0x60006000', 'I2C1': '0x7000c000'}, dict(re.findall(r'#define (CAR|I2C1)\s+(0x[0-9a-f]+)u', text))
        )
        found: dict[str, list[int]] = {'CAR': [], 'I2C1': []}
        for base, offset in re.findall(r'\] = \{(CAR|I2C1) \+ (0x[0-9a-f]+)', text):
            found[base].append(int(offset, 16))
        self.assertEqual({k: sorted(v) for k, v in ALLOWED_OFFSETS.items()}, {k: sorted(v) for k, v in found.items()})

    def test_the_car_rows_are_read_only(self) -> None:
        for row in re.findall(
            r'^\s*\[T1V3_CAR_\w+\] = \{CAR[^}]*\}', (LOADER / 't1v3_regs.c').read_text(), re.MULTILINE
        ):
            self.assertTrue(row.rstrip('}').rstrip().endswith(', R'), row)

    def test_the_write_allowlist_lists_exactly_the_reviewed_values(self) -> None:
        text = (LOADER / 't1v3_regs.c').read_text()
        body = text[text.index('switch (reg)') :]
        listed: dict[str, tuple[str, ...]] = {}
        for name, expression in re.findall(r'case (T1V3_\w+):[^\n]*\n\s*return ([^;]+);', body):
            listed[name] = tuple(re.findall(r'(0x[0-9a-f]+u|SWITCH_T1V3_\w+)', expression))
        self.assertEqual(ALLOWED_WRITES, listed)

    def test_no_file_but_the_allowlist_holds_a_device_address(self) -> None:
        for name in (*SOURCES, 'diag.c'):
            if name == 't1v3_regs.c':
                continue
            lines = [ln for ln in (LOADER / name).read_text().splitlines() if DEVICE_LITERAL.search(ln)]
            lines = [ln for ln in lines if not (name == 'diag.c' and ('0x70000804' in ln or '0x7000f9d8' in ln))]
            self.assertEqual([], lines, name)

    def test_the_raw_accesses_are_one_volatile_load_and_one_volatile_store(self) -> None:
        sites = [
            line.strip()
            for line in (LOADER / 't1v3_access.c').read_text().splitlines()
            if '(uintptr_t)' in line and 'volatile' in line
        ]
        self.assertEqual(
            [
                'return *(const volatile uint32_t *)(uintptr_t)address;',
                '*(volatile uint32_t *)(uintptr_t)address = value;',
            ],
            sites,
        )

    def test_no_name_of_another_peripheral_and_no_retry_or_restore_in_the_run(self) -> None:
        for name in SOURCES:
            text = (LOADER / name).read_text().lower()
            for word in ('pwm_csr', 'regulator', 'gpio_out', 'pmc_', 'max77', 'bus_clear', 'retry(', 'restore('):
                self.assertNotIn(word, text, f'{name} mentions {word}')

    def test_every_wait_loop_also_ends_by_count(self) -> None:
        text = (LOADER / 't1v3.c').read_text()
        loops = re.findall(r'for \(unsigned polls = 0;[^\n]*', text)
        self.assertEqual(3, len(loops))
        for loop in loops:
            self.assertIn('POLL_LIMIT', text[text.index(loop) : text.index(loop) + 400], loop)
        self.assertEqual(
            {'SWITCH_T1V3_IDLE_SAMPLES', 'SWITCH_T1V3_POLL_LIMIT'}, set(re.findall(r'unsigned i = 0; i < (\w+)', text))
        )

    def test_the_divisor_is_written_once_and_never_the_old_value(self) -> None:
        text = (LOADER / 't1v3.c').read_text()
        self.assertEqual(1, len(re.findall(r'put\(run, T1V3_I2C1_CLK_DIVISOR', text)))
        self.assertNotIn('DIVISOR_BEFORE);', text.replace('!=', ''))

    def test_no_em_dash_in_the_new_files(self) -> None:
        for name in SOURCES:
            self.assertNotIn('—', (LOADER / name).read_text())


class Package(unittest.TestCase):
    def test_the_entry_is_its_own_and_boots_nothing(self) -> None:
        ini = entry_ini('iOS Diag T1V3', 'ios-diag-t1v3', 'iDiagT1V3')
        self.assertIn('[iOS Diag T1V3]', ini)
        self.assertIn('boot_prefixes=/switchroot/ios-diag-t1v3/', ini)
        self.assertIn('id=iDiagT1V3', ini)
        for other in ('iDiagT1\n', 'iDiagT1R', 'ios-diag-t1/', 'ios-diag-t1r/', 'ios-diag-loader/'):
            self.assertNotIn(other, ini)
        self.assertNotIn('boot=1', ini)

    def test_the_bazel_target_names_match(self) -> None:
        text = Path('boot/BUILD.bazel').read_text()
        block = text[text.index('name = "diag_t1v3"') :]
        block = block[: block.index('\n)\n')]
        for expected in ('entry = "ios-diag-t1v3"', 'ident = "iDiagT1V3"', 'title = "iOS Diag T1V3"', 't1v3 = True'):
            self.assertIn(expected, block)
        self.assertIn('-DSWITCH_DIAG_T1V3=1', block)
        self.assertNotIn('t1 = True', block)
        self.assertNotIn('t1r = True', block)


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump'), 'no LLVM on PATH'
)
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [*diag_images.COPTS, '-DSWITCH_DIAG_T1V3=1']
    INCLUDES: ClassVar[list[str]] = [f'boot/{d}' for d in diag_images.INCLUDES] + ['kits/SwitchBoard']

    def compile(self, scratch: str, source: str, index: int) -> str:
        obj = f'{scratch}/{index}.o'
        command = ['clang', *self.FLAGS, *(f for inc in self.INCLUDES for f in ('-I', inc)), '-c', source, '-o', obj]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode and 'unable to create target' in result.stderr:
            self.skipTest('this clang does not target AArch64')
        self.assertEqual(0, result.returncode, result.stderr)
        return obj

    def link(self, scratch: str, extra: list[str]) -> tuple[str, str]:
        sources = [
            *(f'boot/{s}' for s in diag_images.COMMON + diag_images.T1V3 + diag_images.TAIL),
            *extra,
        ]
        objects = [self.compile(scratch, s, i) for i, s in enumerate(sources)]
        elf, linkmap = f'{scratch}/t1v3.elf', f'{scratch}/t1v3.map'
        linked = subprocess.run(
            ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        self.assertEqual(0, linked.returncode, linked.stderr)
        dis = subprocess.run(
            ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
        ).stdout
        return Path(linkmap).read_text(), dis

    def test_it_links_and_passes_the_t1v3_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            text, dis = self.link(scratch, [])
            self.assertEqual([], inspect_map(text, t1v3=True))
            self.assertEqual([], inspect_disassembly(dis, t1v3=True))
            self.assertTrue(inspect_map(text))

    def test_a_second_user_of_a_raw_access_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nvoid switch_t1v3_write32(uint32_t, uint32_t);\n'
                'void rogue(void) { switch_t1v3_write32(0x60006010u, 0); }\n'
            )
            _, dis = self.link(scratch, [str(rogue)])
            self.assertTrue([p for p in inspect_disassembly(dis, t1v3=True) if p.startswith('rogue: calls')])

    def test_no_t1v3_code_is_in_the_other_images(self) -> None:
        for name, (sources, defines, _flag) in diag_images.IMAGES.items():
            if name == 'diag_t1v3':
                continue
            self.assertFalse([s for s in sources if 't1v3' in s], name)
            self.assertNotIn('-DSWITCH_DIAG_T1V3=1', defines)


def build(names: tuple[str, ...] | list[str], check_all_passed: bool = False) -> dict[str, str]:
    """Return the SHA-256 of each named image, built by diag_images.py in this process."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = diag_images.main(['--only', *names])
    if status:
        raise AssertionError(f'diag_images failed:\n{out.getvalue()}')
    rows = [ln.split() for ln in out.getvalue().splitlines()[1:]]
    if check_all_passed and not all(r[2:] == ['inspection', 'passed'] for r in rows):
        raise AssertionError(out.getvalue())
    return {r[0]: r[1] for r in rows}


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objcopy') and shutil.which('llvm-objdump'),
    'no LLVM on PATH',
)
class OtherImages(unittest.TestCase):
    """The shared diag.c grew T1 v3 blocks. Every other image must still be byte for byte what was recorded."""

    def test_the_recorded_images_are_unchanged(self) -> None:
        lines = HASHES.read_text().splitlines()
        recorded_clang = lines[0].removeprefix('clang: ')
        current = subprocess.run(
            ['clang', '--version'], capture_output=True, text=True, check=True
        ).stdout.splitlines()[0]
        if current != recorded_clang:
            self.skipTest(f'hashes were recorded with {recorded_clang}; this is {current}')
        recorded = {fields[0]: fields[1] for fields in (ln.split() for ln in lines[1:]) if len(fields) >= 2}
        existing = ('diag_loader', 'diag_loader_r', 'diag_loader_sd', 'diag_t1', 'diag_t1r')
        self.assertEqual(set(existing) | {'diag_t1v3'}, set(recorded))
        built = build(existing)
        for name in existing:
            self.assertEqual(recorded[name], built[name], f'{name} changed')

    def test_the_t1v3_image_is_recorded_and_passes_inspection(self) -> None:
        self.assertIn('diag_t1v3', build(['diag_t1v3'], check_all_passed=True))


if __name__ == '__main__':
    unittest.main()
