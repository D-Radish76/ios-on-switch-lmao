"""ERISTA_T2_RUN.md and ERISTA_FIRST_XNU_MAC.md against the code and the build files they name."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
RUN = (ROOT / 'ERISTA_T2_RUN.md').read_text()
MAC = (ROOT / 'ERISTA_FIRST_XNU_MAC.md').read_text()
REPORT = (ROOT / 'boot/diag/loader/t2_report.c').read_text()
HEADERS = '\n'.join(
    (ROOT / name).read_text()
    for name in ('boot/diag/loader/t2_regs.h', 'boot/diag/loader/t2_bus.h', 'boot/diag/t2/t2.h')
)


def stop_codes() -> set[int]:
    return {int(m, 16) for m in re.findall(r'=\s*(0x7[0-9a-f]{2})\b', HEADERS)}


def covered_codes(text: str) -> set[int]:
    covered = {int(m, 16) for m in re.findall(r'`(07[0-9A-F]{2})`', text)}
    for low, high in re.findall(r'`(07[0-9A-F]{2})` to `(07[0-9A-F]{2})`', text):
        covered |= set(range(int(low, 16), int(high, 16) + 1))
    return covered


class RunSheet(unittest.TestCase):
    def test_every_stop_code_of_the_image_is_explained(self) -> None:
        codes = stop_codes()
        self.assertGreaterEqual(len(codes), 30)
        self.assertEqual([], [f'{c:04X}' for c in sorted(codes - covered_codes(RUN))])

    def test_the_codes_after_the_divisor_write_are_not_called_pre_write_codes(self) -> None:
        prelude = (ROOT / 'boot/diag/loader/t2_prelude.c').read_text()
        apply_body = prelude[prelude.index('static int apply_divisor') : prelude.index('int switch_t2_prelude')]
        names = set(re.findall(r'switch_t2_hw_stop\(hw, (SWITCH_T2_[A-Z0-9_]+)\)', apply_body))
        names.add(
            re.search(r'switch_t2_hw_load\(hw, SWITCH_T2_LOAD_APPLY, (SWITCH_T2_[A-Z0-9_]+)\)', apply_body).group(1)
        )
        values = {n: int(v, 16) for n, v in re.findall(r'(SWITCH_T2_[A-Z0-9_]+)\s*=\s*(0x7[0-9a-f]{2})', HEADERS)}
        after = {values[n] for n in names}
        self.assertEqual({0x70A, 0x70B, 0x70F, 0x714, 0x717}, after)
        rows = {line.split('|')[1]: line for line in RUN.splitlines() if line.startswith('| `07')}
        after_row = next(line for key, line in rows.items() if '`070A`' in key)
        before_row = next(line for key, line in rows.items() if '`0701` to `0709`' in key)
        self.assertEqual(after, covered_codes(after_row.split('|')[1]))
        self.assertIn('after the divisor write', after_row)
        self.assertEqual(set(), covered_codes(before_row.split('|')[1]) & after)
        prelude_codes = {v for v in values.values() if v < 0x720} - {0x710}
        self.assertEqual(prelude_codes, covered_codes(before_row.split('|')[1]) | after)
        self.assertIn('Rows 1 to 7 run before write 1', RUN)
        self.assertNotIn('all before write 1', RUN)

    def test_the_screen_texts_it_quotes_are_the_loaders(self) -> None:
        texts = (
            'BACK IN SHUTDOWN',
            'SHUTDOWN BIT CLEAR',
            'CONVERSION NOT DONE IN 100 MS. NO RETRY',
            'NOT AN ERISTA. NO T2 REGISTER TOUCHED',
            'REMOTE DIODE OPEN OR READING AT THE RANGE EDGE',
            'THE TWO REMOTE HIGH READS DIFFER',
            'ONE SENSOR WRITE AT MOST',
            'ACCESS REFUSED BY THE ALLOWLIST. LOADER BUG',
            'T2 COMPLETE CODE',
        )
        for text in texts:
            self.assertIn(text, RUN, text)
            self.assertIn(text, REPORT + (ROOT / 'boot/diag/loader/diag.c').read_text(), text)

    def test_the_timing_constants_are_the_codes(self) -> None:
        for name, value in (
            ('T2_MIN_US', 31000),
            ('T2_BOUND_US', 100000),
            ('T2_FIRST_POLL_US', 2000),
            ('T2_POLL_STEP_US', 5000),
        ):
            self.assertRegex(HEADERS, rf'{name}\s*=\s*{value}\b')
        for phrase in ('31 ms', '100 ms', 'about 2 ms', 'every 5 ms', '50 s', '15 s'):
            self.assertIn(phrase, RUN)

    def test_the_one_write_and_the_fixed_forms(self) -> None:
        self.assertRegex(HEADERS, r'T2_ONE_SHOT\s*=\s*0x0f')
        for phrase in (
            'pointer `0F`, value `00`',
            '`CNFG` `2802`',
            '`CNFG` `2A02`',
            '`0000000F`',
            'No CAR',
            'no CAR register',
        ):
            self.assertIn(
                phrase.replace('No CAR', 'no CAR register') if phrase == 'No CAR' else phrase,
                RUN.replace('No CAR', 'no CAR register'),
            )

    def test_the_decode_example_is_the_codes_formula(self) -> None:
        self.assertIn('Sixteenths = high x 16 + (low >> 4)', RUN)
        self.assertEqual(552, 0x22 << 4 | 0x80 >> 4)
        self.assertEqual(34.5, 552 / 16)
        self.assertIn('34 + 8/16 = 34.5 C', RUN)
        self.assertIn('return (int)((unsigned)high << 4 | (low >> 4));', (ROOT / 'boot/diag/t2/t2.c').read_text())

    def test_the_holds_the_records_and_the_retry_ban(self) -> None:
        phrases = (
            'ran once on 2026-11-26',
            'not approved',
            '`boot` stays 0',
            'the feature mask stays 0',
            'T3, T4, the full D1 and every XNU run stay on HOLD',
            'before selecting the entry',
            '`boot/diag/erista-t2-run1.txt`',
            'No second run',
            'a new written approval',
            'Nyx cannot answer it afterwards',
        )
        for phrase in phrases:
            self.assertIn(phrase, RUN, phrase)

    def test_no_em_dash(self) -> None:
        self.assertNotIn('—', RUN)
        self.assertNotIn('—', MAC)


class HostCount(unittest.TestCase):
    def test_the_write_count_formula_is_pinned_by_a_host_test(self) -> None:
        self.assertIn('142 + 9n', RUN)
        test = (ROOT / 'boot/diag/loader/test/t2_bus_test.cpp').read_text()
        self.assertIn('142u + 9u * polls', test)
        self.assertIn('2u + 9u * reads + 5u', test)


class MacChecklist(unittest.TestCase):
    READABLE = ('boot', 'tools/package/test')  # the BUILD files this test has as data

    @staticmethod
    def label_exists(label: str) -> bool:
        package, _, name = label.removeprefix('//').partition(':')
        if package not in MacChecklist.READABLE:
            return True
        text = (ROOT / package / 'BUILD.bazel').read_text()
        return name == 'all' or f'name = "{name}"' in text

    def test_every_target_it_names_exists(self) -> None:
        labels = set(re.findall(r'(?<![-\w/])(//[a-z_/]*:[A-Za-z0-9_./]+)', MAC + RUN))
        missing = sorted(
            label for label in labels if not self.label_exists(label) and not label.startswith('//install:prebuilt')
        )
        self.assertEqual([], missing)

    def test_the_decisions_and_holds_are_stated(self) -> None:
        phrases = (
            'D1 is not a condition',
            'T3 and T4 are not waived',
            '`boot` stays 0',
            'stay on HOLD',
            'Bazel was not run',
            'must fail',
            'never committed',
            'no-cache',
            'single_core',
        )
        for phrase in phrases:
            self.assertIn(phrase, MAC, phrase)

    def test_the_commands_name_the_checker_and_the_seed_tools_that_exist(self) -> None:
        for script in ('first_xnu_check.py', 'erista_seed.py', 'erista_seeded_build.py'):
            self.assertIn(script, MAC)
            self.assertTrue((ROOT / 'tools/package' / script).exists(), script)

    def test_the_boot_row_is_still_zero(self) -> None:
        self.assertIn('X(ERISTA, "erista", 0, 0, 0, 0,', (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text())

    def test_the_margin_figures_it_quotes_match_the_earlier_record(self) -> None:
        record = (ROOT / 'ERISTA_FIRST_XNU_BOOT.md').read_text()
        for figure in ('122880', '0xa001e000', '0xf00', '17543168'):
            self.assertIn(figure, MAC)
            self.assertIn(figure, record)


class SeedIsIgnored(unittest.TestCase):
    def test_the_seed_file_is_git_ignored_and_absent(self) -> None:
        self.assertIn('erista-seed.hex', (ROOT / 'local/.gitignore').read_text())
        self.assertFalse((ROOT / 'local/erista-seed.hex').exists())


if __name__ == '__main__':
    unittest.main()
