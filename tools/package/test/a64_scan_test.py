"""a64_scan.py on synthetic AArch64 listings, and the recorded scans of the pinned BL31 and U-Boot."""

import hashlib
import unittest
from pathlib import Path

from a64_scan import CAR, SOURCE_OFFSETS, Scan, rows

FIRMWARE = Path('bazel/pinned/firmware')
SOURCE_ADDRESSES = {CAR + offset for offset in SOURCE_OFFSETS}


def scan(listing: str) -> list[dict]:
    parsed = rows(listing)
    return Scan(b'', 0, {}).run(parsed)


def recorded(name: str) -> tuple[str, list[list[str]]]:
    lines = Path(f'boot/diag/erista-{name}-scan.txt').read_text().splitlines()
    digest = lines[1].split()[1]
    return digest, [line.split() for line in lines[2:]]


class Synthetic(unittest.TestCase):
    def test_movz_movk_builds_a_car_address(self) -> None:
        found = scan('1000: mov x0, #0x6124\n1004: movk x0, #0x6000, lsl #16\n1008: str w1, [x0]\n')
        self.assertEqual([(CAR + 0x124, 'CAR', 'str')], [(f['address'], f['region'], f['op']) for f in found])
        self.assertIn(found[0]['address'], SOURCE_ADDRESSES)

    def test_an_offset_is_added_to_a_known_base(self) -> None:
        found = scan('1000: mov x1, #0x6000\n1004: movk x1, #0x6000, lsl #16\n1008: ldr w0, [x1, #0x198]\n')
        self.assertEqual([CAR + 0x198], [f['address'] for f in found])

    def test_an_unknown_base_gives_nothing(self) -> None:
        self.assertEqual([], scan('1000: ldr x1, [x2, #0x8]\n1004: str w0, [x1, #0x124]\n'))

    def test_a_call_records_a_device_address_argument(self) -> None:
        found = scan('1000: mov x0, #0x6124\n1004: movk x0, #0x6000, lsl #16\n1008: bl 0x2000\n')
        self.assertEqual({'x0': CAR + 0x124}, found[0]['arguments'])

    def test_a_call_clears_caller_saved_registers(self) -> None:
        found = scan('1000: mov x0, #0x6124\n1004: movk x0, #0x6000, lsl #16\n1008: bl 0x2000\n100c: str w1, [x0]\n')
        self.assertEqual(['call'], [f['op'] for f in found])

    def test_a_periph_number_in_an_argument_is_recorded(self) -> None:
        found = scan('1000: mov w0, #0x36\n1004: bl 0x2000\n')
        self.assertEqual({'x0': 0x36}, found[0]['arguments'])

    def test_writeback_moves_the_base(self) -> None:
        found = scan(
            '1000: mov x1, #0x6000\n1004: movk x1, #0x6000, lsl #16\n1008: ldr w0, [x1, #0x124]!\n100c: ldr w0, [x1]\n'
        )
        self.assertEqual([CAR + 0x124] * 2, [f['address'] for f in found])


class Recorded(unittest.TestCase):
    def test_the_hashes_match_the_pinned_binaries(self) -> None:
        for name in ('bl31', 'bl33'):
            self.assertEqual(hashlib.sha256((FIRMWARE / f'{name}.bin').read_bytes()).hexdigest(), recorded(name)[0])

    def test_no_access_or_call_argument_hits_a_clock_source_register(self) -> None:
        for name in ('bl31', 'bl33'):
            for row in recorded(name)[1]:
                if row[1] == 'call':
                    shown = [int(part.split('=')[1], 16) for part in row[3:]]
                    self.assertFalse(SOURCE_ADDRESSES & set(shown), row)
                else:
                    self.assertNotIn(int(row[3], 16), SOURCE_ADDRESSES, row)

    def test_bl31_touches_car_only_at_reset_enable_and_policy_offsets(self) -> None:
        offsets = {int(row[3], 16) - CAR for row in recorded('bl31')[1] if row[2] == 'CAR'}
        self.assertTrue(offsets)
        self.assertFalse(offsets & set(SOURCE_OFFSETS))

    def test_bl33_has_no_assigned_clock_property(self) -> None:
        data = (FIRMWARE / 'bl33.bin').read_bytes()
        self.assertEqual(0, data.count(b'assigned-clock'))
        self.assertEqual(0, (FIRMWARE / 'bl31.bin').read_bytes().count(b'assigned-clock'))


if __name__ == '__main__':
    unittest.main()
