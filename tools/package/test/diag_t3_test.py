"""T3: the fan rotation step. 20 allowlisted reads, six reviewed writes of three registers, readback, off sequence.

The image is built offline and not approved to run. The eleven earlier images, T3-R included, must still be what
boot/diag/erista-diag-image-hashes.txt records.
"""

import ctypes
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

import diag_images
from diag_loader_inspect import T3_ADDRESSES, inspect_disassembly, inspect_image, inspect_map

ROOT = Path('.')
LOADER = ROOT / 'boot/diag/loader'
HASHES = (ROOT / 'boot/diag/erista-diag-image-hashes.txt').read_text()
SOURCES = (
    't3.c', 't3.h', 't3_tach.c', 't3_regs.c', 't3_gate.c', 't3_access.c', 't3_report.c', 't3_report.h', 't3_settle.c',
)  # fmt: skip
DEVICE_LITERAL = re.compile(r'0x[5-7][0-9a-f]{7}u?\b', re.IGNORECASE)
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T3_MAP = (
    MAP + ' switch_t3_run_on_hardware switch_t3_run switch_t3_access switch_t3_read32 switch_t3_write32'
    ' switch_t3_may_read switch_t3_may_write switch_t3_address switch_t3_state_check switch_t3_count_window'
    ' switch_t3_verdict switch_t3_spent'
)
EARLIER = {
    'diag_loader': '9f2bcd7e', 'diag_loader_r': 'c101d9c3', 'diag_loader_sd': '78c9d7a1', 'diag_t1': '51acd940',
    'diag_t1r': 'ab4651dc', 'diag_t1v3': 'de7ea6af', 'diag_t1t': 'c81b53ad', 'diag_t2': '7f5a91a4',
    'diag_state_v2': 'dfecb72f', 'diag_t4r': 'de467deb', 'diag_t3r': '075d6816',
}  # fmt: skip
ADDRESSES = [
    0x60006004, 0x60006010, 0x7000A010, 0x7000320C, 0x700031EC, 0x7000304C, 0x700031A8, 0x6000D000, 0x6000D010,
    0x6000D020, 0x6000D700, 0x6000D710, 0x6000D720, 0x6000D408, 0x6000D418, 0x6000D438, 0x6000D504, 0x6000D514,
    0x7000E444, 0x7000E4E4,
]  # fmt: skip
WRITES = {
    (9, 0x00000020), (9, 0x00000000), (2, 0x80B90000), (2, 0x81000000), (3, 0x00000001), (3, 0x00000074),
}  # (register index, value)


def function(name: str, *lines: str) -> str:
    body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
    return f'0000000000000100 <{name}>:\n{body}\n'


@unittest.skipUnless(shutil.which('clang'), 'no clang on PATH')
class Model(unittest.TestCase):
    lib: ClassVar[ctypes.CDLL]
    scratch: ClassVar[tempfile.TemporaryDirectory[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.scratch = tempfile.TemporaryDirectory()
        out = f'{cls.scratch.name}/t3.so'
        sources = [LOADER / n for n in ('t3.c', 't3_tach.c', 't3_regs.c')]
        done = subprocess.run(
            ['clang', '-shared', '-fPIC', '-O1', f'-I{LOADER}', *map(str, sources), '-o', out],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        if done.returncode:
            raise unittest.SkipTest(f'the host library did not build: {done.stderr}')
        cls.lib = ctypes.CDLL(out)
        cls.lib.switch_t3_address.restype = ctypes.c_uint32
        cls.lib.switch_t3_address.argtypes = [ctypes.c_uint]
        cls.lib.switch_t3_may_read.argtypes = [ctypes.c_uint, ctypes.c_uint]
        cls.lib.switch_t3_may_write.argtypes = [ctypes.c_uint, ctypes.c_uint32]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.scratch.cleanup()

    def test_the_read_allowlist_is_the_20_words_at_32_bits_and_nothing_else(self) -> None:
        allowed = []
        for reg in range(64):
            for width in range(65):
                if self.lib.switch_t3_may_read(reg, width):
                    self.assertEqual(32, width)
                    allowed.append(self.lib.switch_t3_address(reg))
        self.assertEqual(sorted(ADDRESSES), sorted(allowed))
        for reg in range(20, 64):
            self.assertEqual(0, self.lib.switch_t3_address(reg))

    def test_the_write_allowlist_is_the_six_words_and_nothing_else(self) -> None:
        probes = {0, 1, 0x20, 0x74, 0x101, 0x80A00000, 0x80B90000, 0x81000000, 0xFFFFFFFF, 0x80000000}
        allowed = {
            (reg, value)
            for reg in range(64)
            for value in probes
            if self.lib.switch_t3_may_write(reg, value)
        }
        self.assertEqual(WRITES, allowed)
        written = {self.lib.switch_t3_address(reg) for reg, _ in allowed}
        self.assertEqual({0x6000D020, 0x7000A010, 0x7000320C}, written)

    def test_no_write_reaches_a_register_hekate_sets_up_once_or_the_clock_and_reset_words(self) -> None:
        for reg in (0, 1, 4, 5, 6, 10, 11, 12, 13, 14, 18, 19):
            for value in (0, 1, 0x10, 0x20, 0x74, 0xFFFFFFFF):
                self.assertFalse(self.lib.switch_t3_may_write(reg, value), (reg, value))


class Sources(unittest.TestCase):
    def test_every_device_address_literal_is_in_the_register_table_only(self) -> None:
        for name in SOURCES:
            if name == 't3_regs.c':
                continue
            lines = [ln for ln in (LOADER / name).read_text().splitlines() if DEVICE_LITERAL.search(ln)]
            self.assertEqual([], lines, name)
        found = {int(m.rstrip('uU'), 16) for m in DEVICE_LITERAL.findall((LOADER / 't3_regs.c').read_text())}
        self.assertEqual(set(ADDRESSES), found)

    def test_the_six_words_are_the_designed_ones(self) -> None:
        text = (LOADER / 't3.h').read_text()
        for line in (
            '#define SWITCH_T3_RAIL_ON_WORD   0x00000020u',
            '#define SWITCH_T3_PWM_DUTY51     0x80b90000u',
            '#define SWITCH_T3_PWM_OFF_WORD   0x81000000u',
            '#define SWITCH_T3_ROUTE_PWM_WORD 0x00000001u',
            '#define SWITCH_T3_PARK_PAD_WORD  0x00000074u',
        ):
            self.assertIn(line, text)
        self.assertNotIn('0x80a00000', (LOADER / 't3.h').read_text().lower())
        self.assertEqual((236 - 51) << 16 | 0x80000000, 0x80B90000)

    def test_the_only_device_stores_and_loads_are_the_two_raw_functions_in_t3_access(self) -> None:
        sites = [
            ln.strip() for ln in (LOADER / 't3_access.c').read_text().splitlines() if '(uintptr_t)' in ln and 'volatile' in ln
        ]
        self.assertEqual(
            [
                'return *(const volatile uint32_t *)(uintptr_t)address;',
                '*(volatile uint32_t *)(uintptr_t)address = value;',
            ],
            sites,
        )
        for name in SOURCES:
            if name != 't3_access.c':
                self.assertNotRegex((LOADER / name).read_text(), r'\(uintptr_t\)|\*\(volatile', name)
        gate = (LOADER / 't3_gate.c').read_text()
        self.assertLess(gate.index('switch_t3_may_write(reg, value)'), gate.index('switch_t3_write32(address, value)'))
        self.assertLess(gate.index('switch_t3_may_read(reg, T3_WIDTH)'), gate.index('switch_t3_read32(address)'))

    def test_the_logic_never_names_a_device_address_or_the_cpu_registers(self) -> None:
        for name in ('t3.c', 't3_tach.c', 't3_report.c'):
            self.assertNotRegex((LOADER / name).read_text(), r'\bwfe\b|\bwfi\b|\bsmc\b|\bmsr\b|\bmrs\b|\bmemcpy\(|\bmemset\(', name)

    def test_a_write_is_followed_by_a_readback_and_a_mismatch_never_returns_early_in_the_off_sequence(self) -> None:
        text = (LOADER / 't3.c').read_text()
        write = text[text.index('static int write_checked(') : text.index('static int read_state(')]
        self.assertLess(write.index('switch_t3_may_write'), write.index('io->write('))
        self.assertLess(write.index('io->write('), write.index('io->read('))
        self.assertLess(write.index('io->read('), write.index('SWITCH_T3_READBACK'))
        self.assertEqual(1, len(re.findall(r'io->write\(', text)))
        off = text[text.index('static void off_sequence(') : text.index('static int begin(')]
        self.assertNotRegex(off, r'\breturn\b|\bbreak\b')
        self.assertEqual(1, len(re.findall(r'\bwrite_checked\(io, result, undo', off)))
        self.assertLess(off.index('T3_O_PWM_OFF'), off.index('T3_O_RAIL_OFF'))
        self.assertLess(off.index('T3_O_RAIL_OFF'), off.index('T3_O_PARK_PAD'))

    def test_the_state_is_checked_before_any_write_and_every_write_is_a_named_one(self) -> None:
        text = (LOADER / 't3.c').read_text()
        run = text[text.index('void switch_t3_run(') :]
        self.assertLess(run.index('read_state(io, result)'), run.index('idle_and_rail('))
        self.assertLess(run.index('if (!result->stop)'), run.index('idle_and_rail('))
        for write in ('T3_W_RAIL_ON', 'T3_W_PWM_DUTY', 'T3_W_ROUTE_PWM'):
            self.assertEqual(1, text.count(f'write_checked(io, result, {write})'), write)

    def test_the_rail_limit_is_checked_inside_the_tach_loop_and_between_phases(self) -> None:
        tach = (LOADER / 't3_tach.c').read_text()
        loop = tach[tach.index('for (uint32_t pass') : tach.index('static uint32_t larger(')]
        self.assertLess(loop.index('io->rail_expired'), loop.index('io->window_over'))
        self.assertLess(loop.index('io->window_over'), loop.index('io->read('))
        self.assertIn('SWITCH_T3_RAIL_TIME', loop)
        self.assertIn('SWITCH_T3_SAMPLE_LIMIT', tach)
        text = (LOADER / 't3.c').read_text()
        self.assertGreaterEqual(text.count('rail_ok(io, result)'), 4)

    def test_the_tach_level_is_not_fixed_and_no_restore_by_reset_is_claimed(self) -> None:
        tach = (LOADER / 't3_tach.c').read_text()
        self.assertNotIn('== 0', tach[tach.index('void switch_t3_count_window') : tach.index('static uint32_t larger(')])
        state = tach[tach.index('unsigned switch_t3_state_check') :]
        self.assertNotIn('T3_GPIO_S_IN', state)
        diag = (LOADER / 'diag.c').read_text()
        report = diag[diag.index('static void report_t3(') :]
        report = report[: report.index('\n}\n')]
        self.assertIn('NOT CLAIMED TO RESTORE ANY REGISTER', report)

    def test_the_screen_callback_draws_then_settles_and_the_notes_are_drawn_first(self) -> None:
        text = (LOADER / 'diag.c').read_text()
        show = text[text.index('static int show_t3(') : text.index('static uint64_t t3_window_word(')]
        self.assertLess(show.index('!diag.screen'), show.index('put(LEFT'))
        self.assertLess(show.index('put(LEFT'), show.index('return switch_t3_settle()'))
        report = text[text.index('static void report_t3(') :]
        report = report[: report.index('\n}\n')]
        self.assertLess(report.index('NO NEW LINE FOR 30 S'), report.index('switch_t3_run_on_hardware'))
        for note in ('HOLD POWER', 'NOTHING BUT THE FAN', 'NO JOY-CON AND NO USB'):
            self.assertIn(note, report)

    def test_every_screen_string_uses_only_glyphs_the_font_has(self) -> None:
        font = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-.=/()_? ')
        for name in ('t3_report.c', 't3.c', 't3_tach.c'):
            for literal in re.findall(r'"([^"\\\n]*)"', (LOADER / name).read_text()):
                if literal.endswith('.h'):
                    continue
                self.assertFalse(set(literal) - font, (name, literal))
        diag = (LOADER / 'diag.c').read_text()
        report = diag[diag.index('static void report_t3(') :]
        for literal in re.findall(r'put_text\(\d+, "([^"]*)"', report[: report.index('\n}\n')]):
            self.assertFalse(set(literal) - font, literal)

    def test_files_stay_short_and_dash_free(self) -> None:
        for name in SOURCES:
            text = (LOADER / name).read_text()
            self.assertLess(len(text.splitlines()), 300, name)
            self.assertNotIn('—', text)


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_t3_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, t3=True))
        self.assertEqual([], inspect_map(T3_MAP, t3=True))
        self.assertTrue(inspect_map(T3_MAP))
        self.assertTrue(inspect_map(T3_MAP, t3r=True))

    def test_any_other_stage_in_a_t3_link_is_refused(self) -> None:
        for symbol in ('switch_t1_run', 'switch_t4r_run', 'switch_t2_run', 'switch_s2_run', 'switch_t3r_read32', 'switch_t3r_run'):
            self.assertTrue(inspect_map(f'{T3_MAP} {symbol}', t3=True), symbol)

    def test_the_image_holds_exactly_the_20_register_words_once(self) -> None:
        def flat(words: list[int]) -> bytes:
            return b''.join(w.to_bytes(4, 'little') for w in words)

        listed = sorted(T3_ADDRESSES)
        self.assertEqual(sorted(ADDRESSES), listed)
        self.assertEqual([], inspect_image(flat(listed), t3=True))
        for extra in (0x7000A014, 0x7000E448, 0x6000D404, 0x7000E404, 0x6000D800, 0x60006008):
            self.assertTrue(inspect_image(flat([*listed, extra]), t3=True), hex(extra))
        self.assertTrue(inspect_image(flat(listed[1:]), t3=True))
        self.assertTrue(inspect_image(flat([*listed, listed[0]]), t3=True))
        self.assertTrue(inspect_image(flat([*listed, 0x7000A000]), t3=True), 'PWM_CSR_0, the backlight')

    GOOD = (
        function('switch_diag_read32', 'ldr w0, [x0]', 'ret')
        + function('switch_t3_read32', 'ldr w0, [x0]', 'ret')
        + function('switch_t3_write32', 'str w1, [x0]', 'ret')
        + function('switch_t3_access', 'bl 0x100 <switch_t3_may_read>', 'bl 0x100 <switch_t3_may_write>',
                   'bl 0x100 <switch_t3_read32>', 'bl 0x100 <switch_t3_write32>')
    )  # fmt: skip

    def test_the_raw_pair_is_one_load_and_one_store_called_only_by_the_gate(self) -> None:
        self.assertEqual([], [p for p in inspect_disassembly(self.GOOD, t3=True) if 't3' in p])
        for call in ('switch_t3_read32', 'switch_t3_write32'):
            rogue = self.GOOD + function('rogue', f'b 0x100 <{call}>')
            self.assertTrue([p for p in inspect_disassembly(rogue, t3=True) if p.startswith('rogue: calls')], call)
        loaded = self.GOOD.replace('str w1, [x0]', 'ldr w1, [x0]')
        self.assertTrue([p for p in inspect_disassembly(loaded, t3=True) if 'write32' in p])
        for narrow in ('strh w1, [x0]', 'strb w1, [x0]', 'str x1, [x0]'):
            bad = self.GOOD.replace('str w1, [x0]', narrow)
            self.assertTrue([p for p in inspect_disassembly(bad, t3=True) if 'write32' in p], narrow)

    def test_a_gate_that_skips_an_allowlist_is_refused(self) -> None:
        for check in ('switch_t3_may_read', 'switch_t3_may_write'):
            bad = self.GOOD.replace(f'bl 0x100 <{check}>', 'nop')
            self.assertTrue([p for p in inspect_disassembly(bad, t3=True) if check in p], check)


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump') and shutil.which('llvm-objcopy'),
    'no LLVM on PATH',
)
class RealLink(unittest.TestCase):
    def test_it_links_passes_the_inspection_and_matches_the_recorded_hash(self) -> None:
        import argparse

        tools = argparse.Namespace(clang='clang', ld_lld='ld.lld', objcopy='llvm-objcopy', objdump='llvm-objdump')
        with tempfile.TemporaryDirectory() as scratch:
            try:
                digest, problems = diag_images.build('diag_t3', tools, Path(scratch))
            except subprocess.CalledProcessError as error:
                self.skipTest(f'this toolchain does not build the image: {error}')
            self.assertEqual([], problems)
            self.assertIn(f'diag_t3 {digest} inspection passed', HASHES)


class Isolation(unittest.TestCase):
    def test_the_eleven_earlier_hashes_are_unchanged_and_t3_is_the_twelfth(self) -> None:
        lines = [ln.split() for ln in HASHES.splitlines() if ln.startswith('diag_')]
        self.assertEqual([*EARLIER, 'diag_t3'], [ln[0] for ln in lines])
        recorded = {ln[0]: ln[1] for ln in lines}
        for name, short in EARLIER.items():
            self.assertTrue(recorded[name].startswith(short), name)

    def test_no_t3_code_is_in_the_other_images(self) -> None:
        for name, (sources, defines, _flag) in diag_images.IMAGES.items():
            if name == 'diag_t3':
                continue
            self.assertFalse([s for s in sources if re.search(r'/t3(_|\.)', s)], name)
            self.assertNotIn('-DSWITCH_DIAG_T3=1', defines)

    def test_the_target_is_its_own_and_boots_nothing(self) -> None:
        text = (ROOT / 'boot/BUILD.bazel').read_text()
        block = text[text.index('name = "diag_t3"') :]
        block = block[: block.index('\n)\n')]
        for expected in ('entry = "ios-diag-t3"', 'ident = "iDiagT3F"', 'title = "iOS Diag T3"', 't3 = True', '-DSWITCH_DIAG_T3=1', 'u-boot-script.txt'):
            self.assertIn(expected, block)
        for flag in ('t1 = True', 't1r = True', 't1t = True', 't1v3 = True', 't2 = True', 's2 = True', 't4r = True', 't3r = True', 'reservations', 'boot = '):
            self.assertNotIn(flag, block)
        sources = block[block.index('srcs = [') : block.index('hdrs = [')]
        for source in diag_images.T3:
            self.assertIn(f'":{source}"', sources)
        idents = re.findall(r'ident = "(\w+)"', text)
        self.assertEqual(len(idents), len(set(idents)))

    def test_the_psci_reset_path_is_the_shared_exit(self) -> None:
        self.assertIn('diag/loader/exit.c', diag_images.IMAGES['diag_t3'][0])
        self.assertIn('switch_diag_exit(say);', (LOADER / 'diag.c').read_text())


class SavedAudit(unittest.TestCase):
    def test_the_saved_static_audit_passed_on_the_20_words_and_nothing_else(self) -> None:
        text = (ROOT / 'boot/diag/erista-t3-mmio-audit.txt').read_text()
        self.assertIn('AUDIT PASSED (T3)', text)
        self.assertIn('6. Flat image: register address words match the T3 list', text)
        self.assertIn('switch_t3_read32: ldr w0, [x8]', text)
        self.assertIn('switch_t3_write32: str w1, [x8]', text)
        self.assertNotIn('PROBLEMS', text)
        self.assertNotIn('switch_t3r', text)


class RunSheet(unittest.TestCase):
    TEXT = (ROOT / 'ERISTA_T3_RUN.md').read_text()

    def test_the_sheet_states_the_limits_of_the_evidence(self) -> None:
        for phrase in (
            'not approved to run',
            'not a safety guarantee',
            'detach both joy-con',
            'a reset is not claimed',
            'AUDIT PASSED (T3)',
            'duty 51 is the only duty',
            '15 s at most',
        ):
            self.assertIn(phrase.lower(), self.TEXT.lower())
        self.assertNotRegex(self.TEXT, '[\u2013\u2014]')

    def test_every_stop_code_of_the_header_is_in_the_table(self) -> None:
        header = (LOADER / 't3.h').read_text()
        codes = re.findall(r'= 0x(a0[0-9a-f])', header)
        self.assertEqual(10, len(codes))
        for code in codes:
            self.assertIn(f'`0{code.upper()}`', self.TEXT)


if __name__ == '__main__':
    unittest.main()
