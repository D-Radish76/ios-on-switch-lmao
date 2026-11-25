"""bpmpfw_scan.py on synthetic code, and the recorded scan of the Erista BPMP firmware."""

import re
import struct
import sys
import unittest
from pathlib import Path
from unittest import mock

from bpmpfw_scan import IRAM, SOURCE_OFFSETS, Scan, copy_plan, helpers, main, read_header, region

CAR = 0x60006000
RECORDED = Path('boot/diag/erista-bpmpfw-scan.txt').read_text()
ROWS = [line.split() for line in RECORDED.splitlines() if not line.startswith('#') and not line.startswith('sha256')]
HELPER = (
    '40000100: ldr r3, [r0]\n40000104: bic r3, r3, r1\n40000108: orr r3, r3, r2\n'
    '4000010c: str r3, [r0]\n40000110: bx lr\n'
)


def image(*words: tuple[int, int]) -> bytes:
    data = bytearray(0x400)
    for address, value in words:
        struct.pack_into('<I', data, address - IRAM, value)
    return bytes(data)


def run(dis: str, *words: tuple[int, int]) -> list[dict]:
    pool = image(*words)
    return Scan(pool, helpers(dis)).run(dis)


class Synthetic(unittest.TestCase):
    def test_a_literal_base_and_an_offset_resolve_to_a_source_register(self) -> None:
        dis = '40000000: ldr r2, [pc, #0x10] @ 0x40000018 <x>\n40000004: str r3, [r2, #0x124]\n'
        found = run(dis, (0x40000018, CAR))
        self.assertEqual([(CAR + 0x124, 'CAR', 'str')], [(f['address'], f['region'], f['op']) for f in found])
        self.assertEqual('I2C1', SOURCE_OFFSETS[found[0]['address'] - CAR])

    def test_movw_and_movt_build_an_address(self) -> None:
        dis = '40000000: movw r1, #0x6000\n40000004: movt r1, #0x6000\n40000008: ldr r0, [r1, #0x128]\n'
        found = run(dis)
        self.assertEqual([CAR + 0x128], [f['address'] for f in found])

    def test_an_unknown_base_resolves_nothing(self) -> None:
        self.assertEqual([], run('40000000: str r3, [r5, #0x124]\n'))

    def test_an_address_outside_the_watched_regions_is_ignored(self) -> None:
        dis = '40000000: ldr r2, [pc, #0x10] @ 0x40000018 <x>\n40000004: str r3, [r2, #0x4]\n'
        self.assertEqual([], run(dis, (0x40000018, 0x70006000)))

    def test_a_call_to_a_read_modify_write_helper_gives_the_masks(self) -> None:
        dis = (
            HELPER
            + '40000200: ldr r0, [pc, #0x10] @ 0x40000218 <x>\n40000204: mov r1, #0x80000000\n'
            + '40000208: mov r2, #0x40000000\n4000020c: bl 0x40000100 <h>\n'
        )
        found = run(dis, (0x40000218, CAR + 0xA0))
        self.assertEqual(
            [('call-rmw', CAR + 0xA0, 0x80000000, 0x40000000)],
            [(f['op'], f['address'], f['clear'], f['set']) for f in found],
        )

    def test_a_conditional_branch_named_blt_is_not_a_call(self) -> None:
        dis = (
            HELPER
            + '40000200: ldr r0, [pc, #0x10] @ 0x40000218 <x>\n40000204: mov r1, #1\n40000208: mov r2, #2\n'
            + '4000020c: blt 0x40000100 <h>\n'
        )
        self.assertEqual([], run(dis, (0x40000218, CAR + 0xA0)))

    def test_regions_are_car_i2c_and_pmc(self) -> None:
        self.assertEqual(
            ['CAR', 'I2C', 'PMC', None], [region(a) for a in (CAR + 4, 0x7000D01C, 0x7000E4F8, 0x70006000)]
        )


class Image(unittest.TestCase):
    def test_a_file_without_the_magic_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            read_header(b'\0' * 0x100)

    def test_the_copy_plan_comes_from_the_entry_pool(self) -> None:
        data = bytearray(0x300)
        data[:4] = b'bpmp'
        pool = [0x100, 0x268, 0x40000400, 0x4000C6F0, 0xC558, 0x4000C6F0, 0x4001C828] + [0] * 9
        struct.pack_into('<16I', data, 0x210, *pool)
        self.assertEqual([(0x268, 0x40000400, 0x4000C6F0), (0xC558, 0x4000C6F0, 0x4001C828)], copy_plan(bytes(data)))

    def test_a_hash_mismatch_stops_before_any_analysis(self) -> None:
        path = Path('boot/diag/erista-bpmpfw-scan.txt')
        with mock.patch.object(sys, 'argv', ['x', str(path), '--sha256', '0' * 64]), self.assertRaises(SystemExit):
            main()


class Recorded(unittest.TestCase):
    def test_the_header_names_the_recorded_firmware(self) -> None:
        self.assertIn('sha256 943463950880ae7892a1eb8d3a280b3f86083bca0164c663bf4126210be73ecb size 101780', RECORDED)
        self.assertTrue(ROWS)

    def test_no_resolved_access_touches_a_clock_source_register_of_the_six_i2c_buses(self) -> None:
        sources = {CAR + offset for offset in SOURCE_OFFSETS}
        self.assertEqual([], [row for row in ROWS if row[2] == 'CAR' and int(row[3], 16) in sources])

    def test_the_only_i2c_block_it_touches_is_i2c5(self) -> None:
        addresses = {int(row[3], 16) for row in ROWS if row[2] == 'I2C'}
        self.assertTrue(addresses)
        self.assertEqual({0x7000D000}, {a & ~0xFF for a in addresses})

    def test_pllp_is_written_only_through_helper_calls_that_touch_enable_bypass_and_the_out_registers(self) -> None:
        writes = [row for row in ROWS if row[2] == 'CAR' and int(row[3], 16) in (CAR + 0xA0, CAR + 0xA4, CAR + 0xA8)]
        self.assertTrue(writes)
        for row in writes:
            self.assertTrue(row[1].startswith(('call-rmw', 'ldr')), row)
        base = [row for row in writes if int(row[3], 16) == CAR + 0xA0 and row[1] == 'call-rmw']
        self.assertEqual({'c0000000', '80000000'}, {row[row.index('clear') + 1] for row in base})

    def test_no_em_dash(self) -> None:
        self.assertNotIn('—', RECORDED)
        self.assertIsNone(re.search('—', Path('tools/package/bpmpfw_scan.py').read_text()))


if __name__ == '__main__':
    unittest.main()
