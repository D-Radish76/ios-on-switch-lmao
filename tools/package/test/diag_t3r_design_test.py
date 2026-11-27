"""ERISTA_T3R_DESIGN.md: the read addresses follow hekate's formulas, and the holds are stated."""

import re
import unittest
from pathlib import Path

TEXT = Path('ERISTA_T3R_DESIGN.md').read_text()
GPIO_BASE = 0x6000D000
PORTS = {'A': 0, 'S': 18, 'V': 21, 'CC': 28}
CNF, OE, OUT, IN = 0x00, 0x10, 0x20, 0x30


def gpio(port: int, register: int) -> int:
    return GPIO_BASE + ((port >> 2) << 8) + ((port % 4) << 2) + register


class Addresses(unittest.TestCase):
    def test_gpio_words_follow_the_port_formula(self) -> None:
        wanted = [gpio(PORTS['A'], r) for r in (CNF, OE, OUT)] + [gpio(PORTS['CC'], r) for r in (CNF, OE, OUT)]
        wanted += [gpio(PORTS['S'], r) for r in (CNF, OE, IN)] + [gpio(PORTS['V'], r) for r in (CNF, OE)]
        for address in wanted:
            self.assertIn(f'`0x{address:08X}`', TEXT)

    def test_the_other_blocks(self) -> None:
        pwm, pinmux, pmc = 0x7000A000, 0x70003000, 0x7000E400
        for address in (pwm + 0x10, pinmux + 0x20C, pinmux + 0x1EC, pinmux + 0x4C, pinmux + 0x1A8, pmc + 0x44, pmc + 0xE4):
            self.assertIn(f'`0x{address:08X}`', TEXT)

    def test_eighteen_new_reads(self) -> None:
        rows = re.findall(r'^\| (\d+)(?: to (\d+))?(?:, (\d+))? \| ', TEXT, re.MULTILINE)
        self.assertTrue(rows)
        self.assertEqual('18', rows[-1][0])


class Holds(unittest.TestCase):
    def test_read_only_and_holds(self) -> None:
        for phrase in ('no store to a device address', '`boot` stays 0', 'T3 and T4 are not waived', 'on HOLD', 'NO WRITES'):
            self.assertIn(phrase, TEXT)
        self.assertNotRegex(TEXT, '[\\u2013\\u2014]')
        self.assertNotRegex(TEXT, '(?i)ioswitch')
        self.assertLess(len(TEXT.splitlines()), 500)

    def test_named_documents_exist(self) -> None:
        for name in set(re.findall(r'`(ERISTA_[A-Z0-9_]+\.md)`', TEXT)):
            self.assertTrue(Path(name).is_file(), name)


if __name__ == '__main__':
    unittest.main()
