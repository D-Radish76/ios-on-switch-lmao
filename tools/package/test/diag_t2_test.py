"""T2: the inspection, a real link, the device addresses its sources may hold, the entry, and that T1 v3 is untouched.

The T2 image has its own register table, allowlist, gate and raw accesses. Nothing of T1 v3 is linked into it, and every
other image must still be what boot/diag/erista-diag-image-hashes.txt records.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

import diag_images
from diag_loader import entry_ini
from diag_loader_inspect import T1V3_ADDRESSES, inspect_disassembly, inspect_image, inspect_map

LOADER = Path('boot/diag/loader')
HASHES = Path('boot/diag/erista-diag-image-hashes.txt')
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T2_MAP = (
    MAP + ' switch_t2_run_on_hardware switch_t2_prelude switch_t2_run switch_t2_access switch_t2_read32'
    ' switch_t2_write32 switch_t2_may_write'
)
SOURCES = ('t2_image.c', 't2_image.h', 't2_prelude.c', 't2_prelude.h', 't2_bus.c', 't2_bus.h', 't2_regs.c', 't2_regs.h',
           't2_gate.c', 't2_access.c', 't2_report.c', 't2_report.h')  # fmt: skip
DEVICE_LITERAL = re.compile(r'0x[5-7][0-9a-f]{7}u?\b', re.IGNORECASE)
ALLOWED_OFFSETS = {
    'CAR': (0x004, 0x010, 0x124, 0x0A0),
    'I2C1': (0x00, 0x04, 0x0C, 0x1C, 0x68, 0x6C, 0x8C, 0x94, 0x98),
}


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_t2_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, t2=True))
        self.assertEqual([], inspect_map(T2_MAP, t2=True))
        self.assertTrue(inspect_map(T2_MAP))

    def test_any_other_stage_in_a_t2_link_is_refused(self) -> None:
        for symbol in ('switch_t1_run', 'switch_t1r_read32', 'switch_t1v3_run', 'switch_t1t_run'):
            self.assertTrue(inspect_map(f'{T2_MAP} {symbol}', t2=True), symbol)

    def test_the_t2_image_holds_exactly_the_reviewed_register_words(self) -> None:
        def flat(words: list[int]) -> bytes:
            return b''.join(w.to_bytes(4, 'little') for w in words)

        listed = sorted(T1V3_ADDRESSES)
        self.assertEqual([], inspect_image(flat(listed), t2=True))
        self.assertTrue(inspect_image(flat([*listed, 0x60006008]), t2=True))
        self.assertTrue(inspect_image(flat(listed[1:]), t2=True))
        self.assertTrue(inspect_image(flat([*listed, listed[0]]), t2=True))

    def test_each_raw_access_is_one_instruction_and_only_the_gate_calls_it(self) -> None:
        def function(name: str, *lines: str) -> str:
            body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
            return f'0000000000000100 <{name}>:\n{body}\n'

        good = (
            function('switch_diag_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_t2_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_t2_write32', 'mov w8, w0', 'str w1, [x8]', 'ret')
            + function('switch_t2_access', 'bl 0x100 <switch_t2_read32>', 'b 0x100 <switch_t2_write32>')
        )
        mine = [p for p in inspect_disassembly(good, t2=True) if 'switch_t2' in p]
        self.assertEqual([], mine)
        rogue = good + function('rogue', 'b 0x100 <switch_t2_write32>')
        self.assertTrue([p for p in inspect_disassembly(rogue, t2=True) if p.startswith('rogue: calls')])


class Sources(unittest.TestCase):
    def test_every_register_address_is_in_car_or_i2c1_and_is_a_reviewed_one(self) -> None:
        text = (LOADER / 't2_regs.c').read_text()
        self.assertEqual(
            {'CAR': '0x60006000', 'I2C1': '0x7000c000'}, dict(re.findall(r'#define (CAR|I2C1)\s+(0x[0-9a-f]+)u', text))
        )
        found: dict[str, list[int]] = {'CAR': [], 'I2C1': []}
        for base, offset in re.findall(r'\] = \{(CAR|I2C1) \+ (0x[0-9a-f]+)', text):
            found[base].append(int(offset, 16))
        self.assertEqual({k: sorted(v) for k, v in ALLOWED_OFFSETS.items()}, {k: sorted(v) for k, v in found.items()})
        for row in re.findall(r'^\s*\[T2_HW_CAR_\w+\] = \{CAR[^}]*\}', text, re.MULTILINE):
            self.assertTrue(row.rstrip('}').rstrip().endswith(', R'), row)

    def test_the_write_allowlist_lists_exactly_the_reviewed_values(self) -> None:
        text = (LOADER / 't2_regs.c').read_text()
        body = text[text.index('switch (reg)') :]
        self.assertEqual(['0x98u', '0x99u'], re.findall(r'(0x9[89]u)', body[body.index('CMD_ADDR0') : body.index('CMD_DATA1')]))
        pointers = re.findall(r'case (0x[0-9a-f]{2}):', body)
        self.assertEqual(sorted(['0xfe', '0x03', '0x11', '0x12', '0x02', '0x01', '0x10', '0x00', '0x15', '0x0f']), sorted(pointers))
        cnfg = body[body.index('T2_HW_I2C1_CNFG') : body.index('T2_HW_I2C1_CLK_DIVISOR')]
        self.assertEqual(['0x2800u', '0x2840u', '0x2a00u', '0x2a40u', '0x2802u', '0x2a02u'], re.findall(r'0x2[0-9a-f]{3}u', cnfg))
        self.assertIn('return value == SWITCH_T2_DIVISOR_AFTER;', body)
        self.assertIn('value == SWITCH_T2_LOAD_APPLY || value == SWITCH_T2_LOAD_TRANSFER', body)

    def test_no_file_but_the_allowlist_holds_a_device_address(self) -> None:
        for name in (*SOURCES, 'diag.c'):
            if name == 't2_regs.c':
                continue
            lines = [ln for ln in (LOADER / name).read_text().splitlines() if DEVICE_LITERAL.search(ln)]
            lines = [ln for ln in lines if not (name == 'diag.c' and ('0x70000804' in ln or '0x7000f9d8' in ln))]
            self.assertEqual([], lines, name)

    def test_the_raw_accesses_are_one_volatile_load_and_one_volatile_store(self) -> None:
        sites = [
            line.strip()
            for line in (LOADER / 't2_access.c').read_text().splitlines()
            if '(uintptr_t)' in line and 'volatile' in line
        ]
        self.assertEqual(
            [
                'return *(const volatile uint32_t *)(uintptr_t)address;',
                '*(volatile uint32_t *)(uintptr_t)address = value;',
            ],
            sites,
        )

    def test_nothing_of_t1v3_and_no_other_peripheral(self) -> None:
        for name in SOURCES:
            text = (LOADER / name).read_text().lower()
            for word in ('t1v3', 'pwm_csr', 'regulator', 'gpio_out', 'pmc_', 'max77', 'bus_clear', 'retry(', 'restore(', 'wdt', 'thermtrip'):
                self.assertNotIn(word, text, f'{name} mentions {word}')

    def test_there_is_one_register_write_site_and_it_checks_the_stop_code_first(self) -> None:
        text = (LOADER / 't2_bus.c').read_text()
        put = text[text.index('void switch_t2_hw_put(') : text.index('uint64_t switch_t2_hw_now(')]
        self.assertLess(put.index('hw->stop'), put.index('may_write'))
        self.assertLess(put.index('hw->stop'), put.index('io->write'))
        for name in SOURCES:
            self.assertEqual(1 if name == 't2_bus.c' else 0, len(re.findall(r'io->write\(', (LOADER / name).read_text())), name)

    def test_the_divisor_is_written_once_and_the_sensor_is_written_once(self) -> None:
        prelude = (LOADER / 't2_prelude.c').read_text()
        self.assertEqual(1, len(re.findall(r'put\(hw, T2_HW_I2C1_CLK_DIVISOR', prelude)))
        sequence = Path('boot/diag/t2/t2.c').read_text()
        self.assertEqual(1, len(re.findall(r'bus->write\(', sequence)))
        bus = (LOADER / 't2_bus.c').read_text()
        self.assertIn('reg != T2_ONE_SHOT || value != 0', bus)

    def test_every_loop_also_ends_by_count(self) -> None:
        for name in ('t2_bus.c', 't2_prelude.c'):
            text = (LOADER / name).read_text()
            for loop in re.findall(r'(?:for \(unsigned polls = 0;|while \(!\(switch_t2_hw_get|for \(polls = 0;)[^\n]*', text):
                self.assertIn('POLL_LIMIT', text[text.index(loop) : text.index(loop) + 500], loop)

    def test_the_pins_are_the_words_t1t_measured(self) -> None:
        header = (LOADER / 't2_regs.h').read_text()
        fixture = Path('boot/diag/erista-t1t-run1.txt').read_text()
        measured = {m[1]: int(m[2], 16) for m in re.finditer(r'^(I2C1_TIMING_[01])=([0-9A-F]{8})$', fixture, re.MULTILINE)}
        for name, key in (('SWITCH_T2_TIMING_0_PIN', 'I2C1_TIMING_0'), ('SWITCH_T2_TIMING_1_PIN', 'I2C1_TIMING_1')):
            self.assertEqual(int(re.search(rf'#define {name}\s+0x([0-9a-f]{{8}})u', header)[1], 16), measured[key])

    def test_no_em_dash_in_the_new_files(self) -> None:
        for name in SOURCES:
            self.assertNotIn('—', (LOADER / name).read_text())


class Package(unittest.TestCase):
    def test_the_entry_is_its_own_and_boots_nothing(self) -> None:
        ini = entry_ini('iOS Diag T2', 'ios-diag-t2', 'iDiagT2')
        self.assertIn('boot_prefixes=/switchroot/ios-diag-t2/', ini)
        for other in ('iDiagT1V3', 'ios-diag-t1v3/', 'ios-diag-loader/'):
            self.assertNotIn(other, ini)
        self.assertNotIn('boot=1', ini)

    def test_the_bazel_target_matches_and_links_no_t1v3_source(self) -> None:
        text = Path('boot/BUILD.bazel').read_text()
        block = text[text.index('name = "diag_t2"') :]
        block = block[: block.index('\n)\n')]
        for expected in ('entry = "ios-diag-t2"', 'ident = "iDiagT2"', 'title = "iOS Diag T2"', 't2 = True', '-DSWITCH_DIAG_T2=1'):
            self.assertIn(expected, block)
        self.assertNotIn('t1v3', block)
        for flag in ('t1 = True', 't1r = True', 't1t = True'):
            self.assertNotIn(flag, block)
        sources = block[block.index('srcs = [') : block.index('hdrs = [')]
        for source in diag_images.T2:
            self.assertIn(f'":{source}"', sources)

    def test_no_t2_code_is_in_the_other_images(self) -> None:
        for name, (sources, defines, _flag) in diag_images.IMAGES.items():
            if name == 'diag_t2':
                continue
            self.assertFalse([s for s in sources if 't2' in s], name)
            self.assertNotIn('-DSWITCH_DIAG_T2=1', defines)


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump') and shutil.which('llvm-objcopy'),
    'no LLVM on PATH',
)
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [*diag_images.COPTS, '-DSWITCH_DIAG_T2=1']
    INCLUDES: ClassVar[list[str]] = [f'boot/{d}' for d in diag_images.INCLUDES] + ['kits/SwitchBoard']

    def link(self, scratch: str, extra: list[str]) -> tuple[str, str, bytes]:
        sources = [*(f'boot/{s}' for s in diag_images.COMMON + diag_images.T2 + diag_images.TAIL), *extra]
        objects = []
        for index, source in enumerate(sources):
            obj = f'{scratch}/{index}.o'
            command = ['clang', *self.FLAGS, *(f for inc in self.INCLUDES for f in ('-I', inc)), '-c', source, '-o', obj]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            if result.returncode and 'unable to create target' in result.stderr:
                self.skipTest('this clang does not target AArch64')
            self.assertEqual(0, result.returncode, result.stderr)
            objects.append(obj)
        elf, linkmap, flat = f'{scratch}/t2.elf', f'{scratch}/t2.map', f'{scratch}/t2.bin'
        linked = subprocess.run(
            ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        self.assertEqual(0, linked.returncode, linked.stderr)
        dis = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True).stdout
        subprocess.run(['llvm-objcopy', '-O', 'binary', elf, flat], check=True)
        return Path(linkmap).read_text(), dis, Path(flat).read_bytes()

    def test_it_links_and_passes_the_t2_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            text, dis, flat = self.link(scratch, [])
            self.assertEqual([], inspect_map(text, t2=True))
            self.assertEqual([], inspect_disassembly(dis, t2=True))
            self.assertEqual([], inspect_image(flat, t2=True))
            self.assertTrue(inspect_map(text))
            self.assertNotIn('switch_t1v3', text)

    def test_a_register_address_added_by_another_file_is_refused_in_the_linked_image(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            extra = Path(scratch) / 'extra.c'
            extra.write_text('const unsigned extra_address = 0x60006008u;\n')
            _, _, flat = self.link(scratch, [str(extra)])
            self.assertTrue([p for p in inspect_image(flat, t2=True) if '0x60006008' in p])

    def test_a_second_user_of_a_raw_access_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nvoid switch_t2_write32(uint32_t, uint32_t);\n'
                'void rogue(void) { switch_t2_write32(0x60006010u, 0); }\n'
            )
            _, dis, _ = self.link(scratch, [str(rogue)])
            self.assertTrue([p for p in inspect_disassembly(dis, t2=True) if p.startswith('rogue: calls')])

    def test_the_recorded_hash_is_what_this_clang_builds(self) -> None:
        lines = HASHES.read_text().splitlines()
        current = subprocess.run(['clang', '--version'], capture_output=True, text=True, check=True).stdout.splitlines()[0]
        if current != lines[0].removeprefix('clang: '):
            self.skipTest('hashes were recorded with another clang')
        recorded = {f[0]: f[1] for f in (ln.split() for ln in lines[1:]) if len(f) >= 2}
        with tempfile.TemporaryDirectory() as scratch:
            digest, problems = diag_images.build('diag_t2', diag_images.argparse.Namespace(
                clang='clang', ld_lld='ld.lld', objcopy='llvm-objcopy', objdump='llvm-objdump'), Path(scratch))
        self.assertEqual([], problems)
        self.assertEqual(recorded['diag_t2'], digest)


if __name__ == '__main__':
    unittest.main()
