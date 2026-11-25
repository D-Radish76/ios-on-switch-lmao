"""D0-R's run on the Erista (boot/diag/erista-d0r-run.log) against bdinfo, the packaged DTB, the script and the ABI."""

import re
import unittest
from itertools import pairwise
from pathlib import Path

from d0_run import areas, field, parse, ranges
from fdt_ranges import reserved_ranges
from loader_abi import EXPORTER_RUN, SD_REFERENCE, SD_REFERENCE_BYTES
from memory_layout import mc_registers, overlaps, parse_bdinfo, regions

ROOT = Path('.')
RUN = parse((ROOT / 'boot/diag/erista-d0r-run.log').read_text())
D0 = parse((ROOT / 'boot/diag/erista-d0-run.log').read_text())
BDINFO = parse_bdinfo((ROOT / 'boot/diag/erista-memory-bdinfo.log').read_text())
MC = mc_registers((ROOT / 'boot/diag/erista-first-run.log').read_text())
DTB = (ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes()
SCRIPT = (ROOT / 'boot/diag/loader/u-boot-script.txt').read_text()
HEADER = RUN['header']
RANGES = ranges(HEADER)
AREAS = areas(HEADER)
FDT_ADDRESS = int(field(HEADER, r'FDT ([0-9A-F]{8}) TOTAL')[1], 16)
FDT_TOTAL = int(field(HEADER, r'TOTAL (\d+)')[1])


def listed_ranges() -> list[tuple[int, int]]:
    return [(s, e) for (kind, _), (s, _, e) in RANGES.items() if kind in ('MAP', 'NODE')]


class Rows(unittest.TestCase):
    def test_every_row_is_start_plus_size_equals_end(self) -> None:
        self.assertEqual(len(RANGES), 6)
        for key, (start, size, end) in RANGES.items():
            self.assertEqual(start + size, end, key)

    def test_the_board_and_entry_lines_equal_the_d0_runs(self) -> None:
        for pattern in (r'BOARD (.*)', r'EL1 (EL=.*)', r'CNTFRQ=(.*)', r'ISAR0 (.*)', r'HIDREV (\S+)', r'ODM4 (\S+)'):
            self.assertEqual(field(HEADER, pattern)[1], field(D0['header'], pattern)[1], pattern)

    def test_the_gic_words_are_the_d0_words(self) -> None:
        self.assertEqual(RUN['gic'], D0['gic'])

    def test_the_summary_counts_agree_with_the_rows(self) -> None:
        self.assertEqual(int(field(HEADER, r'RESERVATIONS (\d+)')[1]), sum(1 for k in RANGES if k[0] == 'MAP'))
        self.assertEqual(int(field(HEADER, r'BANKS (\d+)')[1]), sum(1 for k in RANGES if k[0] == 'BANK'))
        self.assertEqual(int(field(HEADER, r'NODES (\d+)')[1]), 3)
        self.assertEqual(sum(1 for k in RANGES if k[0] == 'NODE'), 1, 'two of the three nodes have no reg')

    def test_the_entry_stack_differs_from_d0s_and_is_in_u_boots_stack(self) -> None:
        sp = int(field(HEADER, r'ENTRY SP ([0-9A-F]{8})')[1], 16)
        self.assertNotEqual(sp, int(field(D0['header'], r'ENTRY SP ([0-9A-F]{8})')[1], 16))
        self.assertLess(BDINFO['sp start'] - sp, 0x10000)


class Banks(unittest.TestCase):
    def test_they_are_the_bdinfo_banks(self) -> None:
        shown = [(RANGES[('BANK', i)][0], RANGES[('BANK', i)][1]) for i in (0, 1)]
        self.assertEqual(shown, BDINFO['banks'])


class ReserveMap(unittest.TestCase):
    def test_entry_0_is_the_source_dtbs_memreserve(self) -> None:
        origin, start, end = reserved_ranges(DTB)[0]
        self.assertEqual(origin, 'memreserve')
        self.assertEqual((RANGES[('MAP', 0)][0], RANGES[('MAP', 0)][2]), (start, end))

    def test_entry_1_is_what_the_script_adds_for_the_framebuffer(self) -> None:
        added = re.findall(r'fdt rsvmem add (0x[0-9A-Fa-f]+) (0x[0-9A-Fa-f]+)', SCRIPT)
        self.assertEqual([(int(a, 16), int(s, 16)) for a, s in added], [RANGES[('MAP', 1)][:2]])

    def test_entry_2_is_the_blob_itself_at_its_total_size(self) -> None:
        self.assertEqual(RANGES[('MAP', 2)][:2], (FDT_ADDRESS, FDT_TOTAL))
        self.assertIn('0x8D000000', SCRIPT)
        self.assertEqual(FDT_TOTAL % 0x1000, 0)
        self.assertGreater(FDT_TOTAL, len(DTB))

    def test_the_node_range_is_the_ramoops_carveout(self) -> None:
        origin, start, end = reserved_ranges(DTB)[1]
        self.assertTrue(origin.endswith('ramoops_carveout'))
        self.assertEqual((RANGES[('NODE', 0)][0], RANGES[('NODE', 0)][2]), (start, end))

    def test_no_two_listed_ranges_overlap(self) -> None:
        spans = sorted(listed_ranges())
        for (_, first_end), (second_start, _) in pairwise(spans):
            self.assertLessEqual(first_end, second_start)


class Areas(unittest.TestCase):
    def test_they_are_the_abis_exporter_run_area_and_sd_reference_page(self) -> None:
        self.assertEqual(AREAS['EXPORTER'][:2], (EXPORTER_RUN[0], EXPORTER_RUN[0] + EXPORTER_RUN[1]))
        self.assertEqual(AREAS['SD REFERENCE'][:2], (SD_REFERENCE, SD_REFERENCE + SD_REFERENCE_BYTES))

    def test_the_loader_called_both_clear_in_bank_0(self) -> None:
        for name, (_, _, verdict) in AREAS.items():
            self.assertTrue(verdict.startswith('CLEAR'), name)
            self.assertTrue(verdict.endswith('BANK 0'), name)

    def test_they_overlap_no_reservation_the_screen_lists(self) -> None:
        for name, (start, end, _) in AREAS.items():
            for r_start, r_end in listed_ranges():
                self.assertFalse(start < r_end and r_start < end, name)

    def test_they_lie_inside_bank_0(self) -> None:
        bank_start, bank_size = BDINFO['banks'][0]
        for name, (start, end, _) in AREAS.items():
            self.assertTrue(bank_start <= start and end <= bank_start + bank_size, name)

    def test_the_exporter_area_is_inside_the_packages_own_staged_kernel_capacity_and_touches_nothing_else(self) -> None:
        start, end, _ = AREAS['EXPORTER']
        touched = overlaps((start, end), regions(BDINFO, MC))
        self.assertEqual([r.name for r in touched], ['staged-kernel-capacity'])
        self.assertTrue(touched[0].start <= start and end <= touched[0].end)

    def test_the_sd_reference_page_touches_no_other_known_region(self) -> None:
        start, end, _ = AREAS['SD REFERENCE']
        known = [r for r in regions(BDINFO, MC) if r.name != 'SD reference page']
        self.assertEqual(overlaps((start, end), known), [])

    def test_the_comma_is_drawn_as_a_question_mark_because_the_font_has_no_comma(self) -> None:
        glyphs = set(re.findall(r"\{'(.)', \{", (ROOT / 'boot/handoff/display.h').read_text()))
        self.assertIn('?', glyphs)
        self.assertNotIn(',', glyphs)
        self.assertEqual({v for _, _, v in AREAS.values()}, {'CLEAR? BANK 0'})


if __name__ == '__main__':
    unittest.main()
