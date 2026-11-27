"""The recorded T4-R run (boot/diag/erista-t4r-run1.txt) replayed through the real C logic and checked against ERISTA_T4R_RESULT.md."""

import ctypes
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

ROOT = Path('.')
LOADER = ROOT / 'boot/diag/loader'
LOG = (ROOT / 'boot/diag/erista-t4r-run1.txt').read_text()
RESULT = (ROOT / 'ERISTA_T4R_RESULT.md').read_text()
STEPS = 14


def values() -> dict[str, str]:
    return dict(re.findall(r'^([a-z0-9_]+)=(.*)$', LOG, re.MULTILINE))


def screen() -> list[str]:
    return re.findall(r'^screen=(.*)$', LOG, re.MULTILINE)


class Result(ctypes.Structure):
    _fields_: ClassVar = [
        ('stop', ctypes.c_int), ('reads', ctypes.c_uint), ('tmrus_first', ctypes.c_uint32),
        ('tmrus_second', ctypes.c_uint32), ('ptv', ctypes.c_uint32), ('config', ctypes.c_uint32),
        ('status', ctypes.c_uint32), ('tmrus_advanced', ctypes.c_int), ('state', ctypes.c_uint), ('why', ctypes.c_uint),
    ]  # fmt: skip


class Line(ctypes.Structure):
    _fields_: ClassVar = [('text', ctypes.c_char * 105), ('length', ctypes.c_uint), ('bad', ctypes.c_int)]


READ = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint)
SHOW = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(Result))


class Io(ctypes.Structure):
    _fields_: ClassVar = [('context', ctypes.c_void_p), ('read', READ), ('show', SHOW)]


class Record(unittest.TestCase):
    def test_the_record_is_complete_and_says_what_it_does_not_hold(self) -> None:
        v = values()
        for key in ('result', 'start_temperature', 'end_temperature', 'screen_stop_code', 'tmrus_advanced_by_us', 'screen_state'):
            self.assertIn(key, v)
        self.assertEqual('READ_AND_RETURN_OK', v['result'])
        self.assertEqual('not measured', v['temperature_during_run'])
        self.assertEqual(STEPS, len(screen()))
        self.assertIn('is not stored', LOG)
        self.assertNotRegex(LOG, '[\\u2013\\u2014]')

    def test_the_tmrus_difference_and_the_temperature_arithmetic(self) -> None:
        v = values()
        first, second = int(v['tmrus_1'], 16), int(v['tmrus_2'], 16)
        self.assertEqual(int(v['tmrus_advanced_by_us']), (second - first) & 0xFFFFFFFF)
        self.assertEqual(1010578, second - first)
        self.assertEqual(18, round((31.3 - 29.5) * 10))
        self.assertEqual('+1.8 C', v['temperature_difference'])

    def test_the_screen_lines_hold_the_recorded_words(self) -> None:
        v = values()
        text = '\n'.join(screen())
        for key, label in (('tmrus_1', 'TMRUS 1'), ('tmr5_ptv', 'TMR5 PTV'), ('wdt0_config', 'WDT0 CONFIG'), ('wdt0_status', 'WDT0 STATUS')):
            self.assertIn(f'{label} READ RESULT {v[key]}', text)
        for address in ('60005010', '60005060', '60005100', '60005104'):
            self.assertIn(f'ADDRESS {address}. 32 BIT', text)
        self.assertEqual(5, text.count('READ BEGIN'))
        self.assertEqual(5, text.count('READ RESULT'))
        self.assertIn(f'COMPLETE. READS {v["screen_reads"]}. STOP {v["screen_stop_code"]}', text)


@unittest.skipUnless(shutil.which('clang'), 'no clang on PATH')
class Replay(unittest.TestCase):
    lib: ClassVar[ctypes.CDLL]
    scratch: ClassVar[tempfile.TemporaryDirectory[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.scratch = tempfile.TemporaryDirectory()
        out = f'{cls.scratch.name}/t4r.so'
        sources = [LOADER / n for n in ('t4r.c', 't4r_regs.c', 't4r_report.c', 'report.c')]
        done = subprocess.run(
            ['clang', '-shared', '-fPIC', '-O1', f'-I{LOADER}', *map(str, sources), '-o', out],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        if done.returncode:
            raise unittest.SkipTest(f'the host library did not build: {done.stderr}')
        cls.lib = ctypes.CDLL(out)
        cls.lib.switch_t4r_run.argtypes = [ctypes.POINTER(Io), ctypes.POINTER(Result)]
        cls.lib.switch_t4r_screen_line.argtypes = [ctypes.c_uint, ctypes.POINTER(Result), ctypes.POINTER(Line)]

    @classmethod
    def tearDownClass(cls) -> None:
        cls.scratch.cleanup()

    def replay(self) -> tuple[Result, list[str], list[int], list[int]]:
        v = values()
        words = {0: [int(v['tmrus_1'], 16), int(v['tmrus_2'], 16)], 1: [int(v['tmr5_ptv'], 16)],
                 2: [int(v['wdt0_config'], 16)], 3: [int(v['wdt0_status'], 16)]}  # fmt: skip
        counts = dict.fromkeys(words, 0)
        lines: list[str] = []
        reads: list[int] = []
        bad: list[int] = []

        def read(_ctx: object, reg: int) -> int:
            reads.append(reg)
            value = words[reg][min(counts[reg], len(words[reg]) - 1)]
            counts[reg] += 1
            return value

        def show(_ctx: object, step: int, res: object) -> int:
            line = Line()
            self.lib.switch_t4r_screen_line(step, res, ctypes.byref(line))
            lines.append(line.text.decode())
            bad.append(line.bad)
            return 1

        io = Io(None, READ(read), SHOW(show))
        result = Result()
        self.lib.switch_t4r_run(ctypes.byref(io), ctypes.byref(result))
        return result, lines, reads, bad

    def test_the_recorded_words_give_the_recorded_screen(self) -> None:
        result, lines, reads, bad = self.replay()
        self.assertEqual(screen(), lines)
        self.assertEqual([0, 1, 2, 3, 0], reads)
        self.assertEqual((0, 5), (result.stop, result.reads))
        self.assertEqual([0] * STEPS, bad)

    def test_the_state_is_idle_with_no_reason(self) -> None:
        result, _lines, _reads, _bad = self.replay()
        self.assertEqual((1, 0), (result.state, result.why))
        self.assertEqual(1, result.tmrus_advanced)


class Result_(unittest.TestCase):
    def test_the_result_document_separates_the_two_kinds_of_success(self) -> None:
        for phrase in ('READ_AND_RETURN_OK', 'Read and return: met', 'Formal pass: not claimed', '1010578', 'IDLE',
                       'does not show', 'not approved'):
            self.assertIn(phrase, RESULT, phrase)
        self.assertNotRegex(RESULT, '[\\u2013\\u2014]')


if __name__ == '__main__':
    unittest.main()
