"""ERISTA_T4A_RESET_PATH.md against the pinned bl31.bin it describes: the code ranges, the ops table and the constants the reset writes."""

import hashlib
import re
import struct
import unittest
from pathlib import Path

ROOT = Path('.')
BL31 = (ROOT / 'bazel/pinned/firmware/bl31.bin').read_bytes()
PATH = (ROOT / 'ERISTA_T4A_RESET_PATH.md').read_text()
DESIGN = (ROOT / 'ERISTA_T4A_DESIGN.md').read_text()
PACKAGE_BL31 = 'dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4'
RANGES = {
    (0x591C, 0x596C): 'd3948ba09bd1856e1565c826865d5bf43a35beae950ff9968b1e25131d352ea3',
    (0x5D74, 0x5E30): '990a54f4f1105fd8df7f8d5148b9b45ce89da4f9f293d49e1860bf9dd04a8eed',
    (0x7160, 0x7184): '397fbb69dcce8de153d2cd1fddda3262b9afc89ebd5d2f99be2082afb6ff6134',
    (0x7E1C, 0x7E64): '50ea0857e839a026d09f3dfd7be1a94b2647606add14a064d6f0391e1d5b4a4c',
}  # fmt: skip


def immediates(start: int, end: int) -> tuple[set[int], set[int]]:
    """The 16 bit immediates of the MOVZ and MOVK instructions in a range, low half and high half."""
    low: set[int] = set()
    high: set[int] = set()
    for offset in range(start, end, 4):
        (word,) = struct.unpack_from('<I', BL31, offset)
        movz, movk = (word & 0x7F800000) == 0x52800000, (word & 0x7F800000) == 0x72800000
        if movz or movk:
            (high if (word >> 21) & 3 == 1 else low).add((word >> 5) & 0xFFFF)
    return low, high


class Binary(unittest.TestCase):
    def test_the_file_is_the_one_on_the_card(self) -> None:
        self.assertEqual(PACKAGE_BL31, hashlib.sha256(BL31).hexdigest())
        self.assertIn(PACKAGE_BL31[:8], PATH)

    def test_the_code_ranges_the_document_names(self) -> None:
        for (start, end), digest in RANGES.items():
            self.assertEqual(digest, hashlib.sha256(BL31[start:end]).hexdigest(), hex(start))
            self.assertIn(f'0x{start:x}', PATH)

    def test_the_ops_table_points_at_the_reset_handler(self) -> None:
        off, reset = struct.unpack_from('<QQ', BL31, 0xB088 + 0x48)
        self.assertEqual((0xFFF0833C, 0xFFF08348), (off, reset))
        self.assertIn('0xb088', PATH)
        self.assertIn('0x8348', PATH)

    def test_the_reset_function_writes_the_documented_words(self) -> None:
        low, high = immediates(0x5D74, 0x5E30)
        for value in (0xE450, 0xE634, 0xE520, 0xDFFF, 0xDC28, 0xF208, 0xC00):
            self.assertIn(value, low, hex(value))
        for value in (0x4001, 0x7000, 0x2E38, 0x6001, 0x6000):
            self.assertIn(value, high, hex(value))
        for word in ('0x2e38dfff', '0x6001dc28', '0x40010000', '0x6000f208', '0x7000e450'):
            self.assertIn(word, PATH, word)

    def test_the_final_reset_sets_main_rst_in_pmc_cntrl(self) -> None:
        low, high = immediates(0x7160, 0x7184)
        self.assertIn(0xE400, low)
        self.assertIn(0x7000, high)
        (word,) = struct.unpack_from('<I', BL31, 0x7174)
        self.assertEqual(0x321C0000, word, 'orr w0, w0, #0x10 (MAIN_RST) on the value read from PMC_CNTRL')


class Documents(unittest.TestCase):
    def test_every_claim_is_marked(self) -> None:
        for mark in ('**confirmed**', '**inferred**', '**unknown**'):
            self.assertIn(mark, PATH)
        for mark in ('**confirmed**', '**inferred**', '**unknown**'):
            self.assertIn(mark, DESIGN)

    def test_the_withdrawn_pass_condition_and_the_period_rule(self) -> None:
        self.assertIn('withdrawn', DESIGN)
        self.assertIn('The period is not taken from existing values', DESIGN)
        self.assertIn('## Conditions before T4-A could be implemented', DESIGN)
        self.assertIn('## The gate: order and state, not only values', DESIGN)
        self.assertEqual(14, len(re.findall(r'^\| \d+ \|', DESIGN, re.MULTILINE)))

    def test_no_dashes_and_nothing_is_called_approved(self) -> None:
        for doc in (PATH, DESIGN):
            self.assertNotRegex(doc, '[\\u2013\\u2014]')
        self.assertIn('Nothing here is built, implemented or approved', DESIGN)
        self.assertIn('T4-R not run again', PATH)


if __name__ == '__main__':
    unittest.main()
