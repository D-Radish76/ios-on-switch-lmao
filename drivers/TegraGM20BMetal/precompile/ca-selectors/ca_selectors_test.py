"""precompile/ca_selectors.py: each IPSW kind's selector table parses, and its header carries the layout."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ca_selectors

TABLES = Path(__file__).resolve().parent
# boot 21's b0f4f3daabb01e2e config with stale arena bytes in 49-51, which no selector compares
STALE = bytes.fromhex(
    '0000000000000000000000000101ff000000000000000000000000000000ff000001030001000100000000000000000000003c003c'
)


class CaSelectorsTest(unittest.TestCase):
    def test_every_kind_has_a_table_of_a_52_byte_config_at_fragment_buffer_0(self):
        for kind in ('21H461', '24B119'):
            s = ca_selectors.parse((TABLES / f'{kind}.txt').read_text())
            self.assertEqual((s.slot, s.length), (0, 52), kind)
            self.assertTrue({49, 50, 51} <= s.padding, kind)

    def test_the_ios27_padding_is_the_bytes_device_dumps_showed_stale_or_always_zero(self):
        s = ca_selectors.parse((TABLES / '24B119.txt').read_text())
        self.assertEqual(sorted(s.padding), [3, 15, 19, 31, 49, 50, 51])
        self.assertEqual(s.padding_mask() & (1 << 49 | 1 << 50 | 1 << 51), 1 << 49 | 1 << 50 | 1 << 51)
        self.assertTrue(any(STALE[i] for i in s.padding))

    def test_the_ios27_table_lists_boot_21s_configs_of_the_three_generic_states(self):
        s = ca_selectors.parse((TABLES / '24B119.txt').read_text())
        self.assertEqual({k for k, _ in s.configs}, {0xB0F4F3DAABB01E2E, 0x64ED02FD5D7A1865, 0x12016B9F2603FAB8})
        self.assertEqual(len(s.configs), 81)
        self.assertIn((0xB0F4F3DAABB01E2E, STALE[:49] + bytes(3)), s.configs)

    def test_ios17_lists_none_until_a_device_draws_them(self):
        self.assertEqual(ca_selectors.parse((TABLES / '21H461.txt').read_text()).configs, [])

    def test_the_header_names_the_layout(self):
        s = ca_selectors.parse('layout slot=2 length=8 padding=1,4-5\n')
        h = ca_selectors.header(s, 'x.txt')
        self.assertIn('kCaSelectorSlot = 2, kCaSelectorLength = 8;', h)
        self.assertIn('kCaSelectorPadding = 0x32ull;', h)

    def test_config_lines_are_kept_and_checked_against_the_layout(self):
        s = ca_selectors.parse('layout slot=0 length=2 padding=\nb0f4f3daabb01e2e 0a0b\n')
        self.assertEqual(s.configs, [(0xB0F4F3DAABB01E2E, b'\x0a\x0b')])
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=2 padding=\nb0f4f3daabb01e2e 0a\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('b0f4f3daabb01e2e 0a0b\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=65 padding=\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=2 padding=1\nb0f4f3daabb01e2e 0a0b\n')
        with self.assertRaises(ValueError):
            ca_selectors.parse('layout slot=0 length=2 padding=\nb0f4f3daabb01e2e 0a0b\nb0f4f3daabb01e2e 0a0b\n')


if __name__ == '__main__':
    unittest.main()
