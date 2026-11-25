"""hekate_bin_check.py on synthetic binaries."""

import struct
import unittest

from hekate_bin_check import I2C_TABLE, LITERALS, arm_mov_30000, check, row, table


class Table(unittest.TestCase):
    def test_a_row_packs_the_bitfield_the_way_the_source_declares_it(self) -> None:
        self.assertEqual(bytes([0x00, 0x03, 0x20, 0x03, 0x24, 0x01, 0xCC, 0x03]), row(0x300, 0x320, 0x124, 12, 3))

    def test_the_table_has_six_rows_and_keeps_i2c5_at_divider_zero(self) -> None:
        self.assertEqual(48, len(table()))
        by_name = {name: div for name, *_, div in I2C_TABLE}
        self.assertEqual((3, 0), (by_name['I2C1'], by_name['I2C5']))


class Check(unittest.TestCase):
    def test_a_binary_with_the_table_and_a_literal_is_reported(self) -> None:
        data = b'\0' * 12 + table() + struct.pack('<I', 0x00030003)
        out = '\n'.join(check('hekate', data))
        self.assertIn('at 0xc', out)
        self.assertIn('literal 0x00030003 PLLP_OUTB value written by hw_init: 1 at 0x3c', out)
        self.assertIn('literal 0xed000000 NYX_STORAGE_ADDR: 0 at none', out)

    def test_a_missing_table_lists_rows_one_by_one(self) -> None:
        data = row(0x300, 0x320, 0x124, 12, 3) + b'\0' * 8
        out = '\n'.join(check('nyx', data))
        self.assertIn('NOT FOUND', out)
        self.assertIn('row I2C1: at 0x0', out)
        self.assertIn('row I2C5: not found', out)

    def test_a_literal_at_an_unaligned_offset_is_not_counted(self) -> None:
        value = LITERALS['nyx']['EXT_PAYLOAD_ADDR']
        out = '\n'.join(check('nyx', b'\0' + struct.pack('<I', value)))
        self.assertIn('0xc0000000 EXT_PAYLOAD_ADDR: 0', out)

    def test_an_arm_mov_of_0x30000_is_found_for_any_register(self) -> None:
        data = struct.pack('<II', 0xE3A03803, 0xE3B02803) + struct.pack('<I', 0xE3A03802)
        self.assertEqual([0, 4], arm_mov_30000(data))

    def test_the_identity_strings_are_reported(self) -> None:
        out = '\n'.join(check('hekate', b'\0\0\0\0hekate v6.5.5'))
        self.assertIn("identity b'hekate v6.5.5': at 0x4", out)
        self.assertIn('not found', '\n'.join(check('hekate', b'')))


if __name__ == '__main__':
    unittest.main()
