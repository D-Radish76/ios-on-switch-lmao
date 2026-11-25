"""The D0 loader's first run on the Erista (boot/diag/erista-d0-run.log) against the tables and earlier measurements."""

import re
import unittest
from pathlib import Path

from d0_run import decode_gic, field, parse
from fdt_ranges import reserved_ranges
from memory_layout import mc_registers, parse_bdinfo

ROOT = Path('.')
RUN = parse((ROOT / 'boot/diag/erista-d0-run.log').read_text())
TABLE = (ROOT / 'boot/diag/loader/mc_table.h').read_text()
FIRST_RUN = mc_registers((ROOT / 'boot/diag/erista-first-run.log').read_text())
BDINFO = parse_bdinfo((ROOT / 'boot/diag/erista-memory-bdinfo.log').read_text())
DTB = (ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes()
SCRIPT = (ROOT / 'boot/diag/loader/u-boot-script.txt').read_text()
FRAMEBUFFER_RESERVATIONS_IN_SCRIPT = len(re.findall(r'fdt rsvmem add', SCRIPT))
GIC = dict(RUN['gic'])


def table_rows() -> list[tuple[int, int]]:
    body = TABLE.split('switch_diag_mc[][2] = {')[1].split('};')[0]
    return [(int(a, 16), int(v, 0)) for a, v in re.findall(r'\{(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+|\d+)\}', body)]


class Header(unittest.TestCase):
    def test_the_board_and_entry_state(self) -> None:
        h = RUN['header']
        self.assertIn('BOARD ERISTA T210B01=0 SKU=0 BOOT=0', h)
        self.assertIn('EL1 STATE PASS', h)
        self.assertEqual(int(field(h, r'CNTFRQ=(\d+)')[1]), 19200000)
        self.assertEqual(int(field(h, r'HIDREV ([0-9A-F]{8})')[1], 16), 0x00022117)
        self.assertEqual(int(field(h, r'ODM4 ([0-9A-F]{8})')[1], 16), 0x00000204)
        self.assertEqual(int(field(h, r'EL=(\d)')[1]), 4)
        self.assertEqual(int(field(h, r'ISAR0 ([0-9A-F]{16})')[1], 16) >> 16 & 0xF, 1)

    def test_the_entry_stack_is_in_u_boots_stack_below_its_start(self) -> None:
        sp = int(field(RUN['header'], r'ENTRY SP ([0-9A-F]{8})')[1], 16)
        self.assertLess(sp, BDINFO['sp start'])
        self.assertLess(BDINFO['sp start'] - sp, 0x10000)

    def test_the_hypervisor_configuration_the_loader_reports(self) -> None:
        self.assertEqual(int(field(RUN['header'], r'HCR=([0-9A-F]+)')[1], 16), 0x80000000)


class Mc(unittest.TestCase):
    def test_the_25_rows_are_the_table_in_order_and_all_same(self) -> None:
        self.assertEqual(len(RUN['mc']), 25)
        self.assertEqual([(a, v) for a, v, _ in RUN['mc']], table_rows())
        self.assertEqual({s for _, _, s in RUN['mc']}, {'SAME'})

    def test_they_equal_the_first_diagnostics_rows(self) -> None:
        for address, value, _ in RUN['mc']:
            self.assertEqual(FIRST_RUN[address], value, hex(address))

    def test_the_summary_line_agrees(self) -> None:
        self.assertIn('MC REGISTERS READ 25 DIFFERENT FROM THE OLED 0', RUN['header'])


class Gic(unittest.TestCase):
    def test_the_addresses_are_the_tables_and_the_normal_loaders(self) -> None:
        listed = [
            int(x, 16) for x in re.findall(r'0x5004[0-9a-f]{4}', TABLE.split('switch_diag_gic[] =')[1].split('}')[0])
        ]
        loader = (ROOT / 'boot/loader/loader.c').read_text()
        normal = [
            int(x, 16)
            for x in re.findall(r'0x5004[0-9a-f]{4}', loader.split('const uint32_t addresses[] = {')[1].split('}')[0])
        ]
        self.assertEqual([a for a, _ in RUN['gic']], listed)
        self.assertEqual(listed, normal)

    def test_the_words_read_as_a_gic_400_with_the_distributor_and_cpu_interface_on(self) -> None:
        g = decode_gic(GIC)
        self.assertEqual(g['product'], 0x020)
        self.assertEqual(g['implementer'], 0x43B)
        self.assertEqual(g['architecture'], 2)
        self.assertEqual((g['interrupt_lines'], g['cpu_interfaces'], g['security_extensions']), (224, 4, 1))
        self.assertEqual(g['distributor_enabled'], 1)
        self.assertEqual(g['priority_mask'], 0x80)


class Fdt(unittest.TestCase):
    def test_the_source_dtb_has_one_memreserve_and_one_ranged_carveout(self) -> None:
        self.assertEqual(
            reserved_ranges(DTB),
            [('memreserve', 0x80000000, 0x80020000), ('/reserved-memory/ramoops_carveout', 0xB0000000, 0xB0200000)],
        )

    def test_the_screen_counts_one_reservation_more_than_the_source_and_the_script_add_explain(self) -> None:
        shown = int(field(RUN['header'], r'RESERVATIONS (\d+)')[1])
        source = sum(1 for origin, _, _ in reserved_ranges(DTB) if origin == 'memreserve')
        self.assertEqual(FRAMEBUFFER_RESERVATIONS_IN_SCRIPT, 1)
        self.assertEqual(shown, 3)
        self.assertEqual(
            shown - source - FRAMEBUFFER_RESERVATIONS_IN_SCRIPT, 1, 'the third entry has no source in this repository'
        )

    def test_the_walk_and_counts(self) -> None:
        self.assertIn('FDT WALK CODE 0 ALL RANGES FREE', RUN['header'])
        self.assertIn('FDT NODES 3 BANKS 2', RUN['header'])
        self.assertEqual(int(field(RUN['header'], r'TOTAL (\d+)')[1]), 172032)
        self.assertGreater(172032, len(DTB))
        self.assertEqual(172032 % 0x1000, 0)


if __name__ == '__main__':
    unittest.main()
