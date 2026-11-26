"""T4-R: four 32 bit TKE reads behind an allowlist, no store, the state classification, the inspection and the other images.

The image is built offline and not approved to run. The nine earlier images must still be what
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
from diag_loader_inspect import T4R_ADDRESSES, inspect_disassembly, inspect_image, inspect_map

ROOT = Path('.')
LOADER = ROOT / 'boot/diag/loader'
HASHES = (ROOT / 'boot/diag/erista-diag-image-hashes.txt').read_text()
SOURCES = ('t4r.c', 't4r.h', 't4r_regs.c', 't4r_gate.c', 't4r_access.c', 't4r_report.c', 't4r_report.h', 't4r_settle.c')
DEVICE_LITERAL = re.compile(r'0x[5-7][0-9a-f]{7}u?\b', re.IGNORECASE)
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
T4R_MAP = (
    MAP + ' switch_t4r_run_on_hardware switch_t4r_run switch_t4r_access switch_t4r_read32 switch_t4r_may_read'
    ' switch_t4r_address switch_t4r_classify switch_t4r_advanced switch_t4r_settle'
)
T4R_STEPS = 14
EARLIER = {
    'diag_loader': '9f2bcd7e', 'diag_loader_r': 'c101d9c3', 'diag_loader_sd': '78c9d7a1', 'diag_t1': '51acd940',
    'diag_t1r': 'ab4651dc', 'diag_t1v3': 'de7ea6af', 'diag_t1t': 'c81b53ad', 'diag_t2': '7f5a91a4',
    'diag_state_v2': 'dfecb72f',
}  # fmt: skip


def function(name: str, *lines: str) -> str:
    body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
    return f'0000000000000100 <{name}>:\n{body}\n'


class Result(ctypes.Structure):
    _fields_: ClassVar = [
        ('stop', ctypes.c_int), ('reads', ctypes.c_uint), ('tmrus_first', ctypes.c_uint32),
        ('tmrus_second', ctypes.c_uint32), ('ptv', ctypes.c_uint32), ('config', ctypes.c_uint32),
        ('status', ctypes.c_uint32), ('tmrus_advanced', ctypes.c_int), ('state', ctypes.c_uint), ('why', ctypes.c_uint),
    ]  # fmt: skip


READ = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint)
SHOW = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(Result))


class Io(ctypes.Structure):
    _fields_: ClassVar = [('context', ctypes.c_void_p), ('read', READ), ('show', SHOW)]


@unittest.skipUnless(shutil.which('clang'), 'no clang on PATH')
class Model(unittest.TestCase):
    lib: ClassVar[ctypes.CDLL]
    scratch: ClassVar[tempfile.TemporaryDirectory[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.scratch = tempfile.TemporaryDirectory()
        out = f'{cls.scratch.name}/t4r.so'
        sources = [LOADER / n for n in ('t4r.c', 't4r_regs.c')]
        done = subprocess.run(
            ['clang', '-shared', '-fPIC', '-O1', f'-I{LOADER}', *map(str, sources), '-o', out],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        if done.returncode:
            raise unittest.SkipTest(f'the host library did not build: {done.stderr}')
        cls.lib = ctypes.CDLL(out)
        cls.lib.switch_t4r_address.restype = ctypes.c_uint32
        cls.lib.switch_t4r_address.argtypes = [ctypes.c_uint]
        cls.lib.switch_t4r_may_read.argtypes = [ctypes.c_uint, ctypes.c_uint]
        cls.lib.switch_t4r_classify.restype = ctypes.c_uint
        cls.lib.switch_t4r_classify.argtypes = [ctypes.c_uint32] * 3 + [ctypes.POINTER(ctypes.c_uint)]
        cls.lib.switch_t4r_advanced.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        cls.lib.switch_t4r_run.argtypes = [ctypes.POINTER(Io), ctypes.POINTER(Result)]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.scratch.cleanup()

    def run_stage(self, words: dict[int, list[int]], refuse: int = -1) -> tuple[Result, list[str]]:
        events: list[str] = []
        counts = dict.fromkeys(words, 0)

        def read(_ctx: object, reg: int) -> int:
            events.append(f'read {reg}')
            value = words[reg][min(counts[reg], len(words[reg]) - 1)]
            counts[reg] += 1
            return value

        def show(_ctx: object, step: int, _res: object) -> int:
            events.append(f'show {step}')
            return int(step != refuse)

        io = Io(None, READ(read), SHOW(show))
        result = Result()
        self.lib.switch_t4r_run(ctypes.byref(io), ctypes.byref(result))
        return result, events

    def test_the_allowlist_is_four_words_at_32_bits(self) -> None:
        allowed = {
            self.lib.switch_t4r_address(reg)
            for reg in range(64)
            for width in range(65)
            if self.lib.switch_t4r_may_read(reg, width)
        }
        self.assertEqual({0x60005010, 0x60005060, 0x60005100, 0x60005104}, allowed)
        self.assertEqual(T4R_ADDRESSES, frozenset(allowed))
        for reg in range(4, 64):
            self.assertEqual(0, self.lib.switch_t4r_address(reg))
            self.assertFalse(self.lib.switch_t4r_may_read(reg, 32))
        for width in (0, 8, 16, 64):
            for reg in range(4):
                self.assertFalse(self.lib.switch_t4r_may_read(reg, width), (reg, width))

    def test_five_reads_in_order_each_between_a_begin_and_a_result(self) -> None:
        result, events = self.run_stage({0: [1_000_000, 1_200_000], 1: [0], 2: [0], 3: [0]})
        self.assertEqual(0, result.stop)
        self.assertEqual(5, result.reads)
        self.assertEqual(5, sum(e.startswith('read') for e in events))
        self.assertEqual(['read 0', 'read 1', 'read 2', 'read 3', 'read 0'], [e for e in events if e.startswith('read')])
        for index, event in enumerate(events):
            if event.startswith('read'):
                self.assertTrue(events[index - 1].startswith('show'), event)
                self.assertTrue(events[index + 1].startswith('show'), event)
        self.assertEqual(1, result.tmrus_advanced)

    def test_a_step_that_does_not_settle_stops_before_the_next_read(self) -> None:
        for step in range(T4R_STEPS):
            result, events = self.run_stage({0: [5, 9], 1: [0], 2: [0], 3: [0]}, refuse=step)
            self.assertEqual(0x902, result.stop, step)
            later = events[events.index(f'show {step}') + 1 :]
            self.assertEqual([], [e for e in later if e.startswith('read')], step)

    def test_the_classification(self) -> None:
        def classify(ptv: int, config: int, status: int) -> tuple[int, int]:
            why = ctypes.c_uint(99)
            return self.lib.switch_t4r_classify(ptv, config, status, ctypes.byref(why)), why.value

        idle, configured, dormant, busy, unknown = 1, 2, 3, 4, 5
        self.assertEqual((idle, 0), classify(0, 0, 0))
        self.assertEqual((configured, 0), classify(0, 0x8035, 0))
        self.assertEqual((dormant, 0), classify(0, 0x8035, 1))
        self.assertEqual((busy, 0), classify(0xC003D08F, 0x8035, 1))
        self.assertEqual((busy, 0), classify(0, 0, 2))
        self.assertEqual((busy, 0), classify(0xC003D08F, 0x8035, (200 << 4) | 1))
        self.assertEqual((busy, 0), classify(0xC003D08F, 0x8035, (3 << 12) | (255 << 4) | 1))
        self.assertEqual((unknown, 3), classify(0x1234, 0, 0))
        self.assertEqual((unknown, 4), classify(0, 0x8036, 1))
        self.assertEqual((unknown, 5), classify(0x80000000, 0, 0))
        self.assertEqual((unknown, 6), classify(0, 0x8035, (5 << 4) | 1))
        self.assertEqual((unknown, 7), classify(0, 0, 1 << 12))
        self.assertEqual((unknown, 8), classify(0, 0, (1 << 12) | (1 << 4)))
        for bit in (2, 3, 14, 20, 31):
            self.assertEqual((unknown, 2), classify(0, 0, 1 << bit), bit)
        for words in ((0xFFFFFFFF, 0, 0), (0, 0xFFFFFFFF, 0), (0, 0, 0xFFFFFFFF)):
            self.assertEqual((unknown, 1), classify(*words))

    def test_tmrus_advance_and_an_unknown_state_still_reads_five_times(self) -> None:
        self.assertTrue(self.lib.switch_t4r_advanced(10, 11))
        self.assertTrue(self.lib.switch_t4r_advanced(0xFFFFFFF0, 0x10))
        self.assertFalse(self.lib.switch_t4r_advanced(5, 5))
        self.assertFalse(self.lib.switch_t4r_advanced(100, 99))
        result, events = self.run_stage({0: [7], 1: [0], 2: [0], 3: [0xFFFFFFFF]})
        self.assertEqual(5, result.reads)
        self.assertEqual(0, result.tmrus_advanced)
        self.assertEqual(0, result.stop)
        self.assertEqual(5, len([e for e in events if e.startswith('read')]))


class Sources(unittest.TestCase):
    def test_the_table_holds_the_four_addresses_and_only_t4r_regs_holds_them(self) -> None:
        text = (LOADER / 't4r_regs.c').read_text()
        self.assertEqual(
            [('T4R_TMRUS', '0x60005010'), ('T4R_TMR5_PTV', '0x60005060'),
             ('T4R_WDT0_CONFIG', '0x60005100'), ('T4R_WDT0_STATUS', '0x60005104')],
            re.findall(r'\[(T4R_\w+)\] = (0x[0-9a-f]{8})u', text),
        )  # fmt: skip
        self.assertEqual(4, len(re.findall(r'0x[0-9a-f]{8}u', text)))
        self.assertIn('width == T4R_WIDTH', text)
        for name in (*SOURCES, 'diag.c'):
            if name == 't4r_regs.c':
                continue
            lines = [ln for ln in (LOADER / name).read_text().splitlines() if DEVICE_LITERAL.search(ln)]
            lines = [ln for ln in lines if not (name == 'diag.c' and ('0x70000804' in ln or '0x7000f9d8' in ln))]
            self.assertEqual([], lines, name)

    def test_no_store_to_a_device_and_no_watchdog_register_beyond_the_two_listed(self) -> None:
        for name in SOURCES:
            text = (LOADER / name).read_text()
            self.assertNotRegex(text, r'write32|may_write|io->write|0xc45a|0x10c|0x108|0x188|0x18c|0x180|0x184', name)
            self.assertNotRegex(text.lower(), r'pmc|thermtrip|i2c|gpio|retry\(|\bwfe\b|\bsmc\b|\bmsr\b', name)
        sites = [
            ln.strip() for ln in (LOADER / 't4r_access.c').read_text().splitlines() if '(uintptr_t)' in ln and 'volatile' in ln
        ]
        self.assertEqual(['return *(const volatile uint32_t *)(uintptr_t)address;'], sites)
        gate = (LOADER / 't4r_gate.c').read_text()
        self.assertLess(gate.index('switch_t4r_may_read(reg, T4R_WIDTH)'), gate.index('switch_t4r_read32(address)'))

    def test_every_read_is_between_a_shown_begin_and_a_shown_result(self) -> None:
        text = (LOADER / 't4r.c').read_text()
        step = text[text.index('static int step_read(') : text.index('static void finish(')]
        self.assertLess(step.index('shown(io, result, begin)'), step.index('get(io, result, reg)'))
        self.assertLess(step.index('get(io, result, reg)'), step.index('shown(io, result, begin + 1)'))
        self.assertEqual(1, len(re.findall(r'\bget\(io, result, reg\)', text)))
        run = text[text.index('void switch_t4r_run(') :]
        order = [run.index(w) for w in ('T4R_TMRUS, &result->tmrus_first', 'T4R_TMR5_PTV', 'T4R_WDT0_CONFIG', 'T4R_WDT0_STATUS', 'T4R_TMRUS, &result->tmrus_second')]
        self.assertEqual(sorted(order), order)

    def test_the_state_is_never_acted_on(self) -> None:
        text = (LOADER / 't4r.c').read_text()
        classify = text[text.index('unsigned switch_t4r_classify(') : text.index('int switch_t4r_advanced(')]
        self.assertNotRegex(classify, r'io->|read\(|write')

    def test_the_screen_callback_draws_then_settles_and_the_notes_are_drawn_first(self) -> None:
        text = (LOADER / 'diag.c').read_text()
        show = text[text.index('static int show_t4r(') : text.index('static void report_t4r(')]
        self.assertLess(show.index('!diag.screen'), show.index('put(LEFT, 9 + step'))
        self.assertLess(show.index('put(LEFT, 9 + step'), show.index('return switch_t4r_settle()'))
        report = text[text.index('static void report_t4r(') :]
        report = report[: report.index('\n}\n')]
        self.assertLess(report.index('NOT LEFT BY ANY TIMEOUT'), report.index('switch_t4r_run_on_hardware'))
        for note in ('NOT LEFT BY ANY TIMEOUT', "AFTER HEKATE'S INIT", 'NOT EVIDENCE OF A WATCHDOG RESET', 'NO WATCHDOG IS SET OR STOPPED'):
            self.assertIn(note, report)
        self.assertNotIn('PMC_RST_STATUS', report)

    def test_the_settle_has_a_barrier_a_bounded_wait_and_no_event_wait(self) -> None:
        text = (LOADER / 't4r_settle.c').read_text()
        self.assertIn('dsb sy', text)
        self.assertIn('SETTLE_ITERATION_LIMIT', text)
        for word in ('wfe', 'wfi', 'smc', 'msr'):
            self.assertNotIn(word, text)

    def test_no_em_dash_in_the_new_files(self) -> None:
        for name in SOURCES:
            self.assertNotIn('—', (LOADER / name).read_text())
        for doc in ('ERISTA_T4R_RUN.md', 'ERISTA_T4R_DESIGN.md'):
            self.assertNotIn('—', (ROOT / doc).read_text())


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_t4r_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, t4r=True))
        self.assertEqual([], inspect_map(T4R_MAP, t4r=True))
        self.assertTrue(inspect_map(T4R_MAP))

    def test_any_other_stage_in_a_t4r_link_is_refused(self) -> None:
        for symbol in ('switch_t1_run', 'switch_t1r_read32', 'switch_t1v3_run', 'switch_t1t_run', 'switch_t2_run', 'switch_s2_run', 'switch_t4r_write32'):
            self.assertTrue(inspect_map(f'{T4R_MAP} {symbol}', t4r=True), symbol)

    def test_the_image_holds_exactly_the_four_register_words_once(self) -> None:
        def flat(words: list[int]) -> bytes:
            return b''.join(w.to_bytes(4, 'little') for w in words)

        listed = sorted(T4R_ADDRESSES)
        self.assertEqual([0x60005010, 0x60005060, 0x60005100, 0x60005104], listed)
        self.assertEqual([], inspect_image(flat(listed), t4r=True))
        for extra in (0x60005108, 0x6000510C, 0x60005180, 0x60005184, 0x60005088, 0x60005064, 0x60005000):
            self.assertTrue(inspect_image(flat([*listed, extra]), t4r=True), hex(extra))
        self.assertTrue(inspect_image(flat(listed[1:]), t4r=True))
        self.assertTrue(inspect_image(flat([*listed, listed[0]]), t4r=True))

    def test_the_raw_read_is_one_word_load_called_only_by_the_gate_and_a_store_or_width_is_refused(self) -> None:
        good = (
            function('switch_diag_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_t4r_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_t4r_access', 'bl 0x100 <switch_t4r_read32>')
        )
        self.assertEqual([], [p for p in inspect_disassembly(good, t4r=True) if 't4r' in p])
        rogue = good + function('rogue', 'b 0x100 <switch_t4r_read32>')
        self.assertTrue([p for p in inspect_disassembly(rogue, t4r=True) if p.startswith('rogue: calls')])
        stored = good.replace('ldr w0, [x0]\n  104: ret', 'str w1, [x0]\n  104: ret', 1)
        self.assertTrue(inspect_disassembly(stored, t4r=True))
        for narrow in ('ldrh w0, [x0]', 'ldrb w0, [x0]', 'ldr x0, [x0]'):
            bad = good.replace('switch_t4r_read32>:\n  100: ldr w0, [x0]', f'switch_t4r_read32>:\n  100: {narrow}')
            self.assertTrue([p for p in inspect_disassembly(bad, t4r=True) if 'switch_t4r_read32' in p], narrow)


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
                digest, problems = diag_images.build('diag_t4r', tools, Path(scratch))
            except subprocess.CalledProcessError as error:
                self.skipTest(f'this toolchain does not build the image: {error}')
            self.assertEqual([], problems)
            self.assertIn(f'diag_t4r {digest} inspection passed', HASHES)


class Isolation(unittest.TestCase):
    def test_the_nine_earlier_hashes_are_unchanged_and_t4r_is_the_tenth(self) -> None:
        lines = [ln.split() for ln in HASHES.splitlines() if ln.startswith('diag_')]
        self.assertEqual(list(EARLIER) + ['diag_t4r'], [ln[0] for ln in lines])
        for name, short in EARLIER.items():
            self.assertTrue(dict((ln[0], ln[1]) for ln in lines)[name].startswith(short), name)

    def test_no_t4r_code_is_in_the_other_images(self) -> None:
        for name, (sources, defines, _flag) in diag_images.IMAGES.items():
            if name == 'diag_t4r':
                continue
            self.assertFalse([s for s in sources if 't4r' in s], name)
            self.assertNotIn('-DSWITCH_DIAG_T4R=1', defines)

    def test_the_target_is_its_own_and_boots_nothing(self) -> None:
        text = (ROOT / 'boot/BUILD.bazel').read_text()
        block = text[text.index('name = "diag_t4r"') :]
        block = block[: block.index('\n)\n')]
        for expected in ('entry = "ios-diag-t4r"', 'ident = "iDiagT4"', 'title = "iOS Diag T4R"', 't4r = True', '-DSWITCH_DIAG_T4R=1', 'u-boot-script.txt'):
            self.assertIn(expected, block)
        for flag in ('t1 = True', 't1r = True', 't1t = True', 't1v3 = True', 't2 = True', 's2 = True', 'reservations', 'boot = '):
            self.assertNotIn(flag, block)
        sources = block[block.index('srcs = [') : block.index('hdrs = [')]
        for source in diag_images.T4R:
            self.assertIn(f'":{source}"', sources)
        idents = re.findall(r'ident = "(\w+)"', text)
        self.assertEqual(len(idents), len(set(idents)))

    def test_the_psci_reset_path_is_the_shared_exit(self) -> None:
        self.assertIn('diag/loader/exit.c', diag_images.IMAGES['diag_t4r'][0])
        self.assertIn('switch_diag_exit(say);', (LOADER / 'diag.c').read_text())


class SavedAudit(unittest.TestCase):
    def test_the_saved_static_audit_passed_on_the_four_words_and_nothing_else(self) -> None:
        text = (ROOT / 'boot/diag/erista-t4r-mmio-audit.txt').read_text()
        self.assertIn('AUDIT PASSED (T4-R)', text)
        self.assertIn('6. Flat image: register address words match the T4-R list', text)
        self.assertIn('switch_t4r_read32: ldr w0, [x8]; called by switch_t4r_access', text)
        self.assertNotIn('PROBLEMS', text)
        section2 = text[text.index('2. Where') : text.index('3. The path')]
        self.assertEqual(['0x70000804', '0x7000f9d8'], re.findall(r'with x0=(0x[0-9a-f]+)', section2))
        self.assertNotIn('switch_t4r_write', text)


class RunSheet(unittest.TestCase):
    def test_the_sheet_states_the_limits_of_the_evidence(self) -> None:
        text = (ROOT / 'ERISTA_T4R_RUN.md').read_text()
        for phrase in (
            'not approved to run',
            'no software timeout leaves it',
            'the state after hekate',
            'not direct evidence that a watchdog ended any run',
            'hekate clears it before the loader runs',
            'AUDIT PASSED (T4-R)',
            'nothing was armed',
        ):
            self.assertIn(phrase.lower(), text.lower())


if __name__ == '__main__':
    unittest.main()
