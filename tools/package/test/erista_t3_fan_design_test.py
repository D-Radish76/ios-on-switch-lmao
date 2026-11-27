"""ERISTA_T3_FAN_DESIGN.md: the write list is small arithmetic and matches the allowlist of the T3 image."""

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
    def test_six_writes_to_three_registers_and_no_duty_76(self) -> None:
        rows = table_rows(r'[WO]\d')
        self.assertEqual(['W1', 'W2', 'W3', 'O1', 'O2', 'O3'], [r[0] for r in rows])
        self.assertNotIn('0x80A00000', TEXT.upper().replace('0X','0x'))
        for row in rows:
            self.assertIn(row[2].strip('`'), ALLOWED_WRITE_ADDRESSES, row[0])

    def test_the_pwm_words_are_the_hekate_formula(self) -> None:
        def word(duty: int) -> str:
            return f'0x{0x80000000 | (236 - duty) << 16:08X}'

        self.assertEqual('0x80B90000', word(51))
        self.assertIn(f'`{word(51)}`', TEXT)
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


class TheWriteListIsTheImages(unittest.TestCase):
    def test_the_allowlist_holds_the_documented_words_and_no_other(self) -> None:
        regs = Path('boot/diag/loader/t3_regs.c').read_text()
        header = Path('boot/diag/loader/t3.h').read_text()
        for word in ('0x80b90000u', '0x81000000u', '0x00000001u', '0x00000074u', '0x00000020u', '0x00000000u'):
            self.assertIn(word, header)
        self.assertEqual(6, len(re.findall(r'\[T3_[WO]_\w+\] = \{', regs)))
        listing = '\n'.join('|'.join(r) for r in table_rows(r'[WO]\d'))
        for word in ('0x00000020', '0x80B90000', '0x00000001', '0x81000000', '0x00000000', '0x00000074'):
            self.assertIn(word, listing)

    def test_the_target_exists_and_is_not_approved(self) -> None:
        build = Path('boot/BUILD.bazel').read_text()
        self.assertIn('name = "diag_t3"', build)
        self.assertIn('Not approved to run', build)

    def test_the_holds_are_stated(self) -> None:
        for phrase in ('not approved to run', '`boot` stays 0', 'feature mask 0', 'T3 and T4 are not waived', 'HOLD', 'not run again'):
            self.assertIn(phrase.lower(), TEXT.lower())

    def test_the_safety_judgement_names_what_stays_open(self) -> None:
        for phrase in ('CC4', 'unknown', 'not a safety guarantee', 'Not claimed', 'rail', 'duty 51 only'):
            self.assertIn(phrase.lower(), TEXT.lower())
        self.assertNotIn('restores every register', TEXT.lower())

    def test_the_tach_level_is_not_fixed_to_zero(self) -> None:
        self.assertIn('not required to be 0', TEXT)
        self.assertNotIn('one sample low', TEXT)


class Unknowns(unittest.TestCase):
    def test_the_unknowns_are_not_dressed_as_findings(self) -> None:
        for phrase in ('inferred', 'unknown', 'not safety guarantees', 'no absolute rpm'):
            self.assertIn(phrase.lower(), TEXT.lower())
        self.assertNotRegex(TEXT, '[–—]')

    def test_named_documents_exist(self) -> None:
        for name in set(re.findall(r'`(ERISTA_[A-Z0-9_]+\.md)`', TEXT)):
            self.assertTrue(Path(name).is_file(), name)


if __name__ == '__main__':
    unittest.main()
