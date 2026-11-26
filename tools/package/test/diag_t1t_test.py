"""T1-T: the read-only inspection, a real link, the addresses its sources hold, the entry and the other images."""

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
from diag_loader_inspect import T1T_ADDRESSES, inspect_disassembly, inspect_image, inspect_map

LOADER = Path('boot/diag/loader')
HASHES = Path('boot/diag/erista-diag-image-hashes.txt')
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T1T_MAP = MAP + ' switch_t1t_run switch_t1t_access switch_t1t_read32 switch_t1t_may_read switch_t1t_address'
READ32 = ('switch_diag_read32', 'ldr w0, [x0]', 'ret')
SOURCES = ('t1t.c', 't1t.h', 't1t_regs.c', 't1t_gate.c', 't1t_access.c', 't1t_report.c', 't1t_report.h')
I2C_OFFSETS = {0x00, 0x1C, 0x68, 0x6C, 0x8C, 0x94, 0x98}
LLVM = (
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objcopy') and shutil.which('llvm-objdump')
)


def function(name: str, *lines: str) -> str:
    body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
    return f'0000000000000100 <{name}>:\n{body}\n'


def image(extra: str = '', raw_read: tuple[str, ...] = ('ldr w0, [x0]', 'ret'), gate: tuple[str, ...] = ()) -> str:
    return (
        function(*READ32)
        + function('switch_t1t_read32', *raw_read)
        + function('switch_t1t_access', 'bl 0x100 <switch_t1t_read32>', *gate)
        + function('switch_t1t_address', 'ldr w0, [x8, x0, lsl #2]', 'ret')
        + function('switch_t1t_may_read', 'cmp w0, #0x2d', 'cset w0, lo', 'ret')
        + extra
    )


def problems_of(text: str) -> list[str]:
    return [p for p in inspect_disassembly(text, t1t=True) if 'switch_t1t' in p or p.startswith('rogue')]


def code(name: str) -> str:
    text = (LOADER / name).read_text()
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    return re.sub(r'//[^\n]*', '', text)


def build(names: list[str]) -> dict[str, str]:
    """Return the SHA-256 of each named image, built by diag_images.py in this process, all inspections passed."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        status = diag_images.main(['--only', *names])
    rows = [ln.split() for ln in out.getvalue().splitlines()[1:]]
    if status or not all(r[2:] == ['inspection', 'passed'] for r in rows):
        raise AssertionError(out.getvalue())
    return {r[0]: r[1] for r in rows}


class Inspection(unittest.TestCase):
    def test_a_single_load_called_by_the_gate_passes(self) -> None:
        self.assertEqual([], problems_of(image()))

    def test_a_raw_read_that_is_more_than_a_load_is_refused(self) -> None:
        self.assertTrue(problems_of(image(raw_read=('ldr w0, [x0]', 'add w0, w0, #1', 'ret'))))

    def test_a_second_caller_of_the_raw_read_is_refused(self) -> None:
        extra = function('rogue', 'bl 0x100 <switch_t1t_read32>')
        self.assertTrue([p for p in problems_of(image(extra)) if p.startswith('rogue: calls')])

    def test_a_store_in_the_gate_that_is_not_a_stack_save_is_refused(self) -> None:
        self.assertTrue(problems_of(image(gate=('str w1, [x8]',))))
        self.assertEqual([], problems_of(image(gate=('str x30, [sp, #8]',))))

    def test_a_store_in_the_address_path_is_refused(self) -> None:
        text = image().replace('cset w0, lo', 'str w0, [x9]')
        self.assertTrue([p for p in problems_of(text) if 'holds a store' in p])

    def test_a_gate_that_never_reaches_the_raw_read_is_refused(self) -> None:
        self.assertTrue(problems_of(image().replace('bl 0x100 <switch_t1t_read32>', 'nop')))

    def test_the_map_wants_the_t1t_symbols_and_only_in_t1t(self) -> None:
        self.assertEqual([], inspect_map(T1T_MAP, t1t=True))
        self.assertTrue(inspect_map(MAP, t1t=True))
        self.assertTrue(inspect_map(T1T_MAP))

    def test_the_map_refuses_a_write_counterpart_and_the_other_stages(self) -> None:
        for name in (
            'switch_t1t_write32',
            'switch_t1t_may_write',
            'switch_t1v3_run',
            'switch_t1r_run',
            'switch_t1_run',
        ):
            self.assertTrue(inspect_map(T1T_MAP + ' ' + name, t1t=True), name)


class FlatImage(unittest.TestCase):
    def words(self, *values: int) -> bytes:
        return b''.join(v.to_bytes(4, 'little') for v in (*sorted(T1T_ADDRESSES), *values))

    def test_the_listed_addresses_once_each_pass(self) -> None:
        self.assertEqual([], inspect_image(self.words(), t1t=True))

    def test_an_unlisted_address_in_any_of_the_three_pages_is_refused(self) -> None:
        for extra in (0x60006008 + 0x100, 0x7000C004, 0x7000D00C, 0x7000C088):
            self.assertTrue([p for p in inspect_image(self.words(extra), t1t=True) if f'{extra:#x}' in p], extra)

    def test_a_missing_or_repeated_address_is_refused(self) -> None:
        listed = sorted(T1T_ADDRESSES)
        short = b''.join(v.to_bytes(4, 'little') for v in listed[1:])
        self.assertTrue(inspect_image(short, t1t=True))
        self.assertTrue(inspect_image(self.words(listed[0]), t1t=True))

    def test_the_list_holds_both_timing_registers_of_both_controllers(self) -> None:
        for base in (0x7000C000, 0x7000D000):
            self.assertIn(base + 0x94, T1T_ADDRESSES)
            self.assertIn(base + 0x98, T1T_ADDRESSES)

    def test_the_list_has_no_command_data_or_bus_clear_register(self) -> None:
        for base in (0x7000C000, 0x7000D000):
            for offset in (0x04, 0x08, 0x0C, 0x10, 0x88):
                self.assertNotIn(base + offset, T1T_ADDRESSES)

    def test_the_list_is_the_regs_table(self) -> None:
        text = code('t1t_regs.c')
        found = {int(m[1], 16) for m in re.finditer(r'(?:CAR|I2C1|I2C5)\s*\+\s*(0x[0-9a-f]+)', text, re.IGNORECASE)}
        self.assertEqual(
            I2C_OFFSETS | {0x004, 0x008, 0x010, 0x014, 0x0A0, 0x124, 0x128, 0x198, 0x1B8, 0x3C4, 0x65C}, found
        )
        self.assertEqual(len(T1T_ADDRESSES), 11 + 2 * len(I2C_OFFSETS))


class Sources(unittest.TestCase):
    def test_no_source_stores_through_a_device_address_or_names_a_write(self) -> None:
        for name in SOURCES:
            text = re.sub(r'"[^"\n]*"', '""', code(name))
            self.assertIsNone(re.search(r'write|\bstr\b|\(\s*volatile\s+uint32_t\s*\*\s*\)', text, re.IGNORECASE), name)

    def test_the_only_dereference_of_a_device_address_is_one_load(self) -> None:
        loads = re.findall(r'\*\(const volatile uint32_t \*\)', code('t1t_access.c'))
        self.assertEqual(1, len(loads))
        self.assertNotIn('volatile uint32_t *)', code('t1t_access.c').replace('const volatile uint32_t *)', ''))

    def test_device_addresses_are_only_in_the_regs_file(self) -> None:
        literal = re.compile(r'0x[5-7][0-9a-f]{7}u?\b', re.IGNORECASE)
        for name in SOURCES:
            self.assertEqual([], literal.findall(code(name)) if name != 't1t_regs.c' else [], name)
        self.assertIn('0x60006000u', code('t1t_regs.c'))

    def test_t1t_does_not_use_the_other_stages(self) -> None:
        for name in SOURCES:
            self.assertIsNone(re.search(r'switch_t1(?:v3|r)?_|T1V3_|T1R_', code(name)), name)

    def test_no_i2c_transfer_registers_are_named(self) -> None:
        for name in SOURCES:
            self.assertIsNone(re.search(r'CMD_ADDR|CMD_DATA|BUS_CLEAR', code(name)), name)


class Package(unittest.TestCase):
    def test_the_entry_is_its_own_and_boots_nothing(self) -> None:
        ini = entry_ini('iOS Diag T1T', 'ios-diag-t1t', 'iDiagT1T')
        self.assertIn('[iOS Diag T1T]', ini)
        self.assertIn('boot_prefixes=/switchroot/ios-diag-t1t/', ini)
        self.assertIn('id=iDiagT1T', ini)
        for other in ('iDiagT1\n', 'iDiagT1R', 'iDiagT1V3', 'ios-diag-t1/', 'ios-diag-t1v3/', 'ios-diag-loader/'):
            self.assertNotIn(other, ini)
        self.assertNotIn('boot=1', ini)

    def test_the_bazel_target_names_match(self) -> None:
        text = Path('boot/BUILD.bazel').read_text()
        block = text[text.index('name = "diag_t1t"') :]
        block = block[: block.index('\n)\n')]
        for expected in ('entry = "ios-diag-t1t"', 'ident = "iDiagT1T"', 'title = "iOS Diag T1T"', 't1t = True'):
            self.assertIn(expected, block)
        self.assertIn('-DSWITCH_DIAG_T1T=1', block)
        self.assertIn('diag/loader/u-boot-script.txt', block)
        for other in ('t1 = True', 't1r = True', 't1v3 = True', 't1v3.c', 't1r.c'):
            self.assertNotIn(other, block)
        for name in SOURCES:
            self.assertIn(f':diag/loader/{name}"', block, name)

    def test_the_sources_list_matches_diag_images(self) -> None:
        self.assertEqual({f'diag/loader/{n}' for n in SOURCES if n.endswith('.c')}, set(diag_images.T1T))


@unittest.skipUnless(LLVM, 'no LLVM on PATH')
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [*diag_images.COPTS, '-DSWITCH_DIAG_T1T=1']
    INCLUDES: ClassVar[list[str]] = [f'boot/{d}' for d in diag_images.INCLUDES] + ['kits/SwitchBoard']

    def compile(self, scratch: str, source: str, index: int) -> str:
        obj = f'{scratch}/{index}.o'
        command = ['clang', *self.FLAGS, *(f for inc in self.INCLUDES for f in ('-I', inc)), '-c', source, '-o', obj]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode and 'unable to create target' in result.stderr:
            self.skipTest('this clang does not target AArch64')
        self.assertEqual(0, result.returncode, result.stderr)
        return obj

    def link(self, scratch: str, extra: list[str]) -> tuple[str, str, bytes]:
        sources = [*(f'boot/{s}' for s in diag_images.COMMON + diag_images.T1T + diag_images.TAIL), *extra]
        objects = [self.compile(scratch, s, i) for i, s in enumerate(sources)]
        elf, linkmap, flat = f'{scratch}/t1t.elf', f'{scratch}/t1t.map', f'{scratch}/t1t.bin'
        linked = subprocess.run(
            ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        self.assertEqual(0, linked.returncode, linked.stderr)
        subprocess.run(['llvm-objcopy', '-O', 'binary', elf, flat], check=True)
        dis = subprocess.run(
            ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
        ).stdout
        return Path(linkmap).read_text(), dis, Path(flat).read_bytes()

    def test_it_links_and_passes_the_t1t_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            text, dis, flat = self.link(scratch, [])
            self.assertEqual([], inspect_map(text, t1t=True))
            self.assertEqual([], inspect_disassembly(dis, t1t=True))
            self.assertEqual([], inspect_image(flat, t1t=True))
            self.assertTrue(inspect_map(text))

    def test_the_linked_binary_has_one_device_load_and_no_store_in_the_t1t_code(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            _, dis, _ = self.link(scratch, [])
            bodies = {}
            current = None
            for line in dis.splitlines():
                if header := re.match(r'^[0-9a-f]+ <([^>]+)>:', line):
                    current = bodies.setdefault(header[1], [])
                elif current is not None and re.match(r'^\s*[0-9a-f]+:', line):
                    current.append(line.split(':', 1)[1].strip())
            loads = [i for i in bodies['switch_t1t_read32'] if i.startswith('ldr')]
            self.assertEqual(1, len(loads))
            for name, body in bodies.items():
                if name in ('switch_t1t_access', 'switch_t1t_read32', 'switch_t1t_address', 'switch_t1t_may_read'):
                    stores = [i for i in body if re.match(r'(?:str|stp|stur|stlr|stxr|stnp|st[1-4])', i)]
                    self.assertFalse([i for i in stores if not re.search(r'\[(?:sp|x29)\b', i)], name)
            self.assertFalse([n for n in bodies if 'write' in n and 't1t' in n])

    def test_a_register_address_added_by_another_file_is_refused_in_the_linked_image(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            extra = Path(scratch) / 'extra.c'
            extra.write_text('const unsigned extra_address = 0x7000c004u;\n')
            _, _, flat = self.link(scratch, [str(extra)])
            self.assertTrue([p for p in inspect_image(flat, t1t=True) if '0x7000c004' in p])

    def test_a_second_user_of_the_raw_read_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nuint32_t switch_t1t_read32(uint32_t);\n'
                'uint32_t rogue(void) { return switch_t1t_read32(0x7000c004u); }\n'
            )
            _, dis, _ = self.link(scratch, [str(rogue)])
            self.assertTrue([p for p in inspect_disassembly(dis, t1t=True) if p.startswith('rogue: calls')])


@unittest.skipUnless(LLVM, 'no LLVM on PATH')
class OtherImages(unittest.TestCase):
    """The shared diag.c grew T1-T blocks. The six earlier images must still be what was recorded."""

    def test_the_recorded_images_are_unchanged(self) -> None:
        lines = HASHES.read_text().splitlines()
        current = subprocess.run(
            ['clang', '--version'], capture_output=True, text=True, check=True
        ).stdout.splitlines()[0]
        if current != lines[0].removeprefix('clang: '):
            self.skipTest(f'hashes were recorded with {lines[0]}; this is {current}')
        recorded = {fields[0]: fields[1] for fields in (ln.split() for ln in lines[1:]) if len(fields) >= 2}
        earlier = ('diag_loader', 'diag_loader_r', 'diag_loader_sd', 'diag_t1', 'diag_t1r', 'diag_t1v3')
        self.assertEqual(set(earlier) | {'diag_t1t', 'diag_t2'}, set(recorded))
        built = build([*earlier, 'diag_t1t'])
        for name in (*earlier, 'diag_t1t'):
            self.assertEqual(recorded[name], built[name], f'{name} changed')

    def test_no_t1t_code_is_in_the_other_images(self) -> None:
        for name, (sources, defines, _flag) in diag_images.IMAGES.items():
            if name == 'diag_t1t':
                continue
            self.assertFalse([s for s in sources if 't1t' in s], name)
            self.assertNotIn('-DSWITCH_DIAG_T1T=1', defines)
