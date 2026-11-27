"""ERISTA_T3_FAN_DESIGN.md: a design only. Its write list is small and arithmetic, and no fan code exists."""

import re
import unittest
from pathlib import Path

DOC = Path('ERISTA_T3_FAN_DESIGN.md')
TEXT = DOC.read_text()
ALLOWED_WRITE_ADDRESSES = {'0x6000D020', '0x7000A010', '0x7000320C'}


def table_rows(prefix: str) -> list[list[str]]:
    rows = []
    for line in TEXT.splitlines():
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if line.startswith('|') and cells and re.fullmatch(prefix, cells[0]):
            rows.append(cells)
    return rows


class WriteList(unittest.TestCase):
    def test_seven_writes_to_three_registers_and_nothing_else(self) -> None:
        rows = table_rows(r'[WO]\d')
        self.assertEqual(['W1', 'W2', 'W3', 'W4', 'O1', 'O2', 'O3'], [r[0] for r in rows])
        for row in rows:
            self.assertIn(row[2].strip('`'), ALLOWED_WRITE_ADDRESSES, row[0])

    def test_the_pwm_words_are_the_hekate_formula(self) -> None:
        def word(duty: int) -> str:
            return f'0x{0x80000000 | (236 - duty) << 16:08X}'

        self.assertEqual('0x80B90000', word(51))
        self.assertEqual('0x80A00000', word(76))
        self.assertIn(f'`{word(51)}`', TEXT)
        self.assertIn(f'`{word(76)}`', TEXT)
        self.assertIn('`0x81000000`', TEXT)

    def test_the_entry_state_is_what_the_first_run_read(self) -> None:
        for word in ('`0x00000000`', '`0x00000074`', '`0x00000059`', '`0x00000101`'):
            self.assertIn(word, TEXT)
        for register in ('0x7000A010', '0x7000320C', '0x700031EC', '0x7000E444', '0x7000E4E4'):
            self.assertIn(register, TEXT)

    def test_clock_reset_and_backlight_registers_are_not_in_the_write_list(self) -> None:
        listing = '\n'.join('|'.join(r) for r in table_rows(r'[WO]\d'))
        for forbidden in ('0x60006004', '0x60006010', '0x7000A000', '0x7000E444', '0x7000E4E4'):
            self.assertNotIn(forbidden, listing)


class NothingIsBuilt(unittest.TestCase):
    def test_no_fan_code_or_target_exists(self) -> None:
        names = {p.name for p in Path('boot/diag/loader').glob('*') if p.is_file()}
        self.assertEqual([], [n for n in names if re.match(r't3[^r]|t3$|fan', n)])
        build = Path('boot/BUILD.bazel').read_text()
        self.assertEqual([], re.findall(r'name = "diag_t3[^r"][^"]*"', build))
        self.assertNotIn('SWITCH_DIAG_T3=', build)
        self.assertNotIn('t3 = True', build)

    def test_the_holds_are_stated(self) -> None:
        for phrase in ('nothing here is implemented', '`boot` stays 0', 'feature mask 0', 'T3 and T4 are not waived', 'HOLD', 'not run again'):
            self.assertIn(phrase.lower(), TEXT.lower())

    def test_the_unknowns_are_not_dressed_as_findings(self) -> None:
        for phrase in ('inferred', 'unknown', 'not safety guarantees', 'no absolute rpm'):
            self.assertIn(phrase.lower(), TEXT.lower())
        self.assertNotRegex(TEXT, '[–—]')

    def test_named_documents_exist(self) -> None:
        for name in set(re.findall(r'`(ERISTA_[A-Z0-9_]+\.md)`', TEXT)):
            self.assertTrue(Path(name).is_file(), name)


if __name__ == '__main__':
    unittest.main()
