"""T3-R: 21 allowlisted 32 bit reads (22 with TMRUS twice), no store, PWM classification, inspection and the other images.

The image is built offline and not approved to run. The ten earlier images must still be what
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
from diag_loader_inspect import T3R_ADDRESSES, inspect_disassembly, inspect_image, inspect_map

ROOT = Path('.')
LOADER = ROOT / 'boot/diag/loader'
HASHES = (ROOT / 'boot/diag/erista-diag-image-hashes.txt').read_text()
SOURCES = ('t3r.c', 't3r.h', 't3r_regs.c', 't3r_gate.c', 't3r_access.c', 't3r_report.c', 't3r_report.h', 't3r_settle.c')
DEVICE_LITERAL = re.compile(r'0x[5-7][0-9a-f]{7}u?\b', re.IGNORECASE)
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T3R_MAP = (
    MAP + ' switch_t3r_run_on_hardware switch_t3r_run switch_t3r_access switch_t3r_read32 switch_t3r_may_read'
    ' switch_t3r_address switch_t3r_sequence switch_t3r_pwm_gate switch_t3r_pwm_classify switch_t3r_expect'
    ' switch_t3r_advanced switch_t3r_settle switch_t3r_budget_spent'
)
EARLIER = {
    'diag_loader': '9f2bcd7e', 'diag_loader_r': 'c101d9c3', 'diag_loader_sd': '78c9d7a1', 'diag_t1': '51acd940',
    'diag_t1r': 'ab4651dc', 'diag_t1v3': 'de7ea6af', 'diag_t1t': 'c81b53ad', 'diag_t2': '7f5a91a4',
    'diag_state_v2': 'dfecb72f', 'diag_t4r': 'de467deb',
}  # fmt: skip
OFF_WORD = 0x81000000
ADDRESSES = [
    0x60005010, 0x60006004, 0x60006010, 0x7000A010, 0x7000320C, 0x700031EC, 0x7000304C, 0x700031A8, 0x6000D000,
    0x6000D010, 0x6000D020, 0x6000D700, 0x6000D710, 0x6000D720, 0x6000D408, 0x6000D418, 0x6000D438, 0x6000D504,
    0x6000D514, 0x7000E444, 0x7000E4E4,
]  # fmt: skip
PWM_REG = 3
STATES = {'not_classified': 0, 'disabled': 1, 'off': 2, 'duty': 3, 'unknown': 4}


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
        out = f'{cls.scratch.name}/t3r.so'
        sources = [LOADER / n for n in ('t3r.c', 't3r_regs.c')]
        done = subprocess.run(
            ['clang', '-shared', '-fPIC', '-O1', f'-I{LOADER}', *map(str, sources), '-o', out],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        if done.returncode:
            raise unittest.SkipTest(f'the host library did not build: {done.stderr}')
        cls.lib = ctypes.CDLL(out)
        cls.lib.switch_t3r_address.restype = ctypes.c_uint32
        cls.lib.switch_t3r_address.argtypes = [ctypes.c_uint]
        cls.lib.switch_t3r_may_read.argtypes = [ctypes.c_uint, ctypes.c_uint]
        cls.lib.switch_t3r_sequence.restype = ctypes.c_uint
        cls.lib.switch_t3r_sequence.argtypes = [ctypes.c_uint]
        cls.lib.switch_t3r_pwm_gate.restype = ctypes.c_uint
        cls.lib.switch_t3r_pwm_gate.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        cls.lib.switch_t3r_pwm_classify.restype = ctypes.c_uint
        cls.lib.switch_t3r_pwm_classify.argtypes = [
            ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
        ]  # fmt: skip

    @classmethod
    def tearDownClass(cls) -> None:
        cls.scratch.cleanup()

    def classify(self, word: int) -> tuple[int, int, int]:
        why, duty = ctypes.c_uint(99), ctypes.c_uint(99)
        state = self.lib.switch_t3r_pwm_classify(word, ctypes.byref(why), ctypes.byref(duty))
        return state, why.value, duty.value

    def test_the_allowlist_is_the_21_words_at_32_bits_and_nothing_else(self) -> None:
        allowed = []
        for reg in range(64):
            for width in range(65):
                if self.lib.switch_t3r_may_read(reg, width):
                    self.assertEqual(32, width)
                    allowed.append(self.lib.switch_t3r_address(reg))
        self.assertEqual(sorted(ADDRESSES), sorted(allowed))
        self.assertEqual(21, len(set(allowed)))
        for reg in range(21, 64):
            self.assertEqual(0, self.lib.switch_t3r_address(reg))

    def test_the_sequence_has_22_reads_with_pmc_last_and_tmrus_first_and_last(self) -> None:
        seq = [self.lib.switch_t3r_sequence(i) for i in range(22)]
        self.assertEqual(0, seq[0])
        self.assertEqual(0, seq[-1])
        self.assertEqual([19, 20], seq[-3:-1])
        self.assertEqual(set(range(21)), set(seq))
        self.assertEqual(1, seq.count(PWM_REG))
        self.assertEqual(21, self.lib.switch_t3r_sequence(22))
        self.assertLess(seq.index(2), seq.index(PWM_REG))
        self.assertLess(seq.index(1), seq.index(PWM_REG))

    def test_the_pwm_gate_is_open_only_with_the_clock_on_and_the_reset_released(self) -> None:
        bit, open_, clock_off, in_reset = 1 << 17, 1, 2, 3
        self.assertEqual(open_, self.lib.switch_t3r_pwm_gate(0, bit))
        self.assertEqual(clock_off, self.lib.switch_t3r_pwm_gate(0, 0))
        self.assertEqual(in_reset, self.lib.switch_t3r_pwm_gate(bit, bit))
        self.assertEqual(clock_off, self.lib.switch_t3r_pwm_gate(bit, 0))
        self.assertEqual(open_, self.lib.switch_t3r_pwm_gate(~bit & 0xFFFFFFFF, bit))
        self.assertEqual(open_, self.lib.switch_t3r_pwm_gate(0, 0xFFFFFFFF ^ 0x1))

    def test_the_pwm_word_is_classified_by_what_hekate_writes(self) -> None:
        self.assertEqual((STATES['off'], 0, 0), self.classify(OFF_WORD))
        self.assertEqual((STATES['disabled'], 0, 0), self.classify(0))
        for field in range(236):
            self.assertEqual((STATES['duty'], 0, 236 - field), self.classify(0x80000000 | field << 16), field)
        unknown = STATES['unknown']
        for word in (0xFFFFFFFF, 0x01000000, 0x00330000, 0x81010000, 0x81000001, 0x80000001, 0x8000FFFF, 0x82000000, 0xC0330000):
            self.assertEqual(unknown, self.classify(word)[0], hex(word))
        self.assertEqual(unknown, self.classify(0x80000000 | 236 << 16)[0])
        for field in range(237, 256):
            self.assertEqual(unknown, self.classify(0x80000000 | field << 16)[0], field)
        reasons = {self.classify(w)[1] for w in (0xFFFFFFFF, 0x01000000, 0x81010000, 0x80000001, 0x80000000 | 236 << 16, 0x80000000 | 240 << 16)}
        self.assertEqual(6, len(reasons))


class Sources(unittest.TestCase):
    def test_every_device_address_literal_is_in_the_register_table_only(self) -> None:
        for name in SOURCES:
            if name == 't3r_regs.c':
                continue
            lines = [ln for ln in (LOADER / name).read_text().splitlines() if DEVICE_LITERAL.search(ln)]
            self.assertEqual([], lines, name)
        found = {int(m.rstrip('uU'), 16) for m in DEVICE_LITERAL.findall((LOADER / 't3r_regs.c').read_text())}
        self.assertEqual(set(ADDRESSES), found)

    def test_no_store_to_a_device_and_no_write_path(self) -> None:
        for name in SOURCES:
            text = (LOADER / name).read_text()
            self.assertNotRegex(text, r'write32|may_write|io->write|\bwfe\b|\bwfi\b|\bsmc\b|\bmsr\b|retry\(', name)
        sites = [
            ln.strip() for ln in (LOADER / 't3r_access.c').read_text().splitlines() if '(uintptr_t)' in ln and 'volatile' in ln
        ]
        self.assertEqual(['return *(const volatile uint32_t *)(uintptr_t)address;'], sites)
        gate = (LOADER / 't3r_gate.c').read_text()
        self.assertLess(gate.index('switch_t3r_may_read(reg, T3R_WIDTH)'), gate.index('switch_t3r_read32(address)'))
        diag = (LOADER / 'diag.c').read_text()
        report = diag[diag.index('static void report_t3r(') :]
        report = report[: report.index('\n}\n')]
        self.assertNotRegex(report, r'write32|\*\(volatile')

    def test_every_read_is_between_a_shown_begin_and_a_shown_result_and_the_only_read_site_is_get(self) -> None:
        text = (LOADER / 't3r.c').read_text()
        step = text[text.index('static int step_read(') : text.index('static void finish(')]
        self.assertLess(step.index('SWITCH_T3R_STEP_BEGIN(index)'), step.index('get(io, result, reg)'))
        self.assertLess(step.index('get(io, result, reg)'), step.index('derive(result, reg,'))
        self.assertLess(step.index('derive(result, reg,'), step.index('SWITCH_T3R_STEP_RESULT(index)'))
        self.assertEqual(1, len(re.findall(r'io->read\(', text)))
        self.assertEqual(1, len(re.findall(r'\bget\(io, result, reg\)', text)))
        self.assertEqual(1, len(re.findall(r'\bstep_read\(', text[text.index('static int read_sequence(') :])))

    def test_the_budget_and_the_all_ones_stop_come_before_the_next_read(self) -> None:
        text = (LOADER / 't3r.c').read_text()
        loop = text[text.index('static int read_sequence(') : text.index('void switch_t3r_run(')]
        self.assertLess(loop.index('io->expired'), loop.index('step_read('))
        self.assertLess(loop.index('SWITCH_T3R_TIME'), loop.index('step_read('))
        self.assertLess(loop.index('step_read('), loop.index('SWITCH_T3R_STOP_ALL_ONES'))
        self.assertIn('reg != T3R_TMRUS', loop)
        self.assertLess(loop.index('T3R_GATE_OPEN'), loop.index('step_read('))

    def test_classification_and_expectation_never_touch_the_device(self) -> None:
        text = (LOADER / 't3r.c').read_text()
        pure = text[text.index('unsigned switch_t3r_pwm_gate(') : text.index('static void derive(')]
        self.assertNotRegex(pure, r'io->|read\(|write')

    def test_the_screen_callback_draws_then_settles_and_the_notes_are_drawn_first(self) -> None:
        text = (LOADER / 'diag.c').read_text()
        show = text[text.index('static int show_t3r(') : text.index('static uint64_t t3r_low_bytes(')]
        self.assertLess(show.index('!diag.screen'), show.index('put(line.right'))
        self.assertLess(show.index('put(line.right'), show.index('return switch_t3r_settle()'))
        report = text[text.index('static void report_t3r(') :]
        report = report[: report.index('\n}\n')]
        self.assertLess(report.index('NO NEW LINE FOR 30 S'), report.index('switch_t3r_run_on_hardware'))
        for note in ('HOLD POWER', 'NOT LEFT BY ANY TIMEOUT', 'AFTER HEKATE AND U-BOOT', 'UNTESTED'):
            self.assertIn(note, report)

    def test_the_settle_has_a_barrier_a_bounded_wait_and_no_event_wait(self) -> None:
        text = (LOADER / 't3r_settle.c').read_text()
        self.assertIn('dsb sy', text)
        self.assertIn('SETTLE_ITERATION_LIMIT', text)
        for word in ('wfe', 'wfi', 'smc', 'msr'):
            self.assertNotIn(word, text)

    def test_no_em_dash_in_the_new_files(self) -> None:
        for name in SOURCES:
            self.assertNotIn('—', (LOADER / name).read_text())
        for doc in ('ERISTA_T3R_RUN.md', 'ERISTA_T3R_DESIGN.md'):
            self.assertNotIn('—', (ROOT / doc).read_text())


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_t3r_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, t3r=True))
        self.assertEqual([], inspect_map(T3R_MAP, t3r=True))
        self.assertTrue(inspect_map(T3R_MAP))

    def test_any_other_stage_in_a_t3r_link_is_refused(self) -> None:
        for symbol in ('switch_t1_run', 'switch_t4r_run', 'switch_t4r_read32', 'switch_t2_run', 'switch_s2_run', 'switch_t3r_write32'):
            self.assertTrue(inspect_map(f'{T3R_MAP} {symbol}', t3r=True), symbol)

    def test_the_image_holds_exactly_the_21_register_words_once(self) -> None:
        def flat(words: list[int]) -> bytes:
            return b''.join(w.to_bytes(4, 'little') for w in words)

        listed = sorted(T3R_ADDRESSES)
        self.assertEqual(sorted(ADDRESSES), listed)
        self.assertEqual([], inspect_image(flat(listed), t3r=True))
        for extra in (0x60005014, 0x7000A014, 0x7000E448, 0x6000D404, 0x7000E404, 0x6000D800):
            self.assertTrue(inspect_image(flat([*listed, extra]), t3r=True), hex(extra))
        self.assertTrue(inspect_image(flat(listed[1:]), t3r=True))
        self.assertTrue(inspect_image(flat([*listed, listed[0]]), t3r=True))

    def test_the_raw_read_is_one_word_load_called_only_by_the_gate_and_a_store_or_width_is_refused(self) -> None:
        good = (
            function('switch_diag_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_t3r_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_t3r_access', 'bl 0x100 <switch_t3r_read32>')
        )
        self.assertEqual([], [p for p in inspect_disassembly(good, t3r=True) if 't3r' in p])
        rogue = good + function('rogue', 'b 0x100 <switch_t3r_read32>')
        self.assertTrue([p for p in inspect_disassembly(rogue, t3r=True) if p.startswith('rogue: calls')])
        stored = good.replace('ldr w0, [x0]\n  104: ret', 'str w1, [x0]\n  104: ret', 1)
        self.assertTrue(inspect_disassembly(stored, t3r=True))
        for narrow in ('ldrh w0, [x0]', 'ldrb w0, [x0]', 'ldr x0, [x0]'):
            bad = good.replace('switch_t3r_read32>:\n  100: ldr w0, [x0]', f'switch_t3r_read32>:\n  100: {narrow}')
            self.assertTrue([p for p in inspect_disassembly(bad, t3r=True) if 'switch_t3r_read32' in p], narrow)


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
                digest, problems = diag_images.build('diag_t3r', tools, Path(scratch))
            except subprocess.CalledProcessError as error:
                self.skipTest(f'this toolchain does not build the image: {error}')
            self.assertEqual([], problems)
            self.assertIn(f'diag_t3r {digest} inspection passed', HASHES)


class Isolation(unittest.TestCase):
    def test_the_ten_earlier_hashes_are_unchanged_and_t3r_is_the_eleventh(self) -> None:
        lines = [ln.split() for ln in HASHES.splitlines() if ln.startswith('diag_')]
        self.assertEqual([*EARLIER, 'diag_t3r'], [ln[0] for ln in lines])
        recorded = {ln[0]: ln[1] for ln in lines}
        for name, short in EARLIER.items():
            self.assertTrue(recorded[name].startswith(short), name)

    def test_no_t3r_code_is_in_the_other_images(self) -> None:
        for name, (sources, defines, _flag) in diag_images.IMAGES.items():
            if name == 'diag_t3r':
                continue
            self.assertFalse([s for s in sources if 't3r' in s], name)
            self.assertNotIn('-DSWITCH_DIAG_T3R=1', defines)

    def test_the_target_is_its_own_and_boots_nothing(self) -> None:
        text = (ROOT / 'boot/BUILD.bazel').read_text()
        block = text[text.index('name = "diag_t3r"') :]
        block = block[: block.index('\n)\n')]
        for expected in ('entry = "ios-diag-t3r"', 'ident = "iDiagT3"', 'title = "iOS Diag T3R"', 't3r = True', '-DSWITCH_DIAG_T3R=1', 'u-boot-script.txt'):
            self.assertIn(expected, block)
        for flag in ('t1 = True', 't1r = True', 't1t = True', 't1v3 = True', 't2 = True', 's2 = True', 't4r = True', 'reservations', 'boot = '):
            self.assertNotIn(flag, block)
        sources = block[block.index('srcs = [') : block.index('hdrs = [')]
        for source in diag_images.T3R:
            self.assertIn(f'":{source}"', sources)
        idents = re.findall(r'ident = "(\w+)"', text)
        self.assertEqual(len(idents), len(set(idents)))

    def test_the_psci_reset_path_is_the_shared_exit(self) -> None:
        self.assertIn('diag/loader/exit.c', diag_images.IMAGES['diag_t3r'][0])
        self.assertIn('switch_diag_exit(say);', (LOADER / 'diag.c').read_text())


class SavedAudit(unittest.TestCase):
    def test_the_saved_static_audit_passed_on_the_21_words_and_nothing_else(self) -> None:
        text = (ROOT / 'boot/diag/erista-t3r-mmio-audit.txt').read_text()
        self.assertIn('AUDIT PASSED (T3-R)', text)
        self.assertIn('6. Flat image: register address words match the T3-R list', text)
        self.assertIn('switch_t3r_read32: ldr w0, [x8]; called by switch_t3r_access', text)
        self.assertNotIn('PROBLEMS', text)
        self.assertNotIn('switch_t3r_write', text)
        section2 = text[text.index('2. Where') : text.index('3. The path')]
        self.assertEqual(['0x70000804', '0x7000f9d8'], re.findall(r'with x0=(0x[0-9a-f]+)', section2))


class RunSheet(unittest.TestCase):
    def test_the_sheet_states_the_limits_of_the_evidence(self) -> None:
        text = (ROOT / 'ERISTA_T3R_RUN.md').read_text()
        for phrase in (
            'not approved to run',
            'no software timeout leaves a hung read',
            'non-secure el1',
            'unconfirmed',
            'no write',
            'AUDIT PASSED (T3-R)',
            'not a safety guarantee',
        ):
            self.assertIn(phrase.lower(), text.lower())


class ResultRecord(unittest.TestCase):
    TEXT = (ROOT / 'ERISTA_T3R_RESULT.md').read_text()

    def test_the_raw_timer_values_give_the_recorded_difference(self) -> None:
        first, last = 0x0680CFDD, 0x06C1B6A5
        self.assertEqual(4253384, (last - first) & 0xFFFFFFFF)
        for text in ('0x0680CFDD', '0x06C1B6A5', '4,253,384'):
            self.assertIn(text, self.TEXT)

    def test_read_success_and_the_display_faults_are_kept_apart(self) -> None:
        for phrase in ('display bug', 'screen layout bug', 'PWM_CSR_1', 'Fixes made offline', 'not run again', 'HOLD'):
            self.assertIn(phrase.lower(), self.TEXT.lower())
        self.assertNotRegex(self.TEXT, '[\u2013\u2014]')

    def test_the_first_run_defects_are_fixed_in_the_code(self) -> None:
        text = (LOADER / 't3r.c').read_text()
        self.assertEqual(1, text.count('result->tmrus_advanced = '))
        derive = text[text.index('static void derive(') : text.index('static int read_sequence(')]
        self.assertIn('result->tmrus_advanced = ', derive)
        self.assertNotIn('tmrus_advanced', text[text.index('static int read_sequence(') :])
        report = (LOADER / 't3r_report.c').read_text()
        self.assertNotIn('place(out, SWITCH_T3R_COLUMN_ROWS', report)
        self.assertEqual(6, len(re.findall(r'\bsummary\(out, \d\);', report)))
        self.assertIn('out->row = SWITCH_T3R_SUMMARY_FIRST_ROW + number;', report)

    def test_every_screen_string_uses_only_glyphs_the_font_has(self) -> None:
        font = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-.=/()_? ')
        for name in ('t3r_report.c', 't3r.c'):
            for literal in re.findall(r'"([^"\\\n]*)"', (LOADER / name).read_text()):
                if literal.endswith('.h'):
                    continue
                self.assertFalse(set(literal) - font, (name, literal))


if __name__ == '__main__':
    unittest.main()
