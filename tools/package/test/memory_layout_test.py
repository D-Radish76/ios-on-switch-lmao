"""The Erista's measured DRAM layout (boot/diag/erista-memory-bdinfo.log) against what the package places."""

import unittest
from itertools import pairwise
from pathlib import Path

from diag_loader import DRAM, FRAMEBUFFER, MAX_IMAGE_END
from loader_abi import NATIVE_FULL_RAM_EXCLUSIONS
from memory_layout import LOADER_SPAN, MIB, banks_end, carveouts, mc_registers, overlaps, parse_bdinfo, regions

ROOT = Path('.')
BDINFO = parse_bdinfo((ROOT / 'boot/diag/erista-memory-bdinfo.log').read_text())
MC = mc_registers((ROOT / 'boot/diag/erista-first-run.log').read_text())
UBOOT_LINK = 0x88110000
FOUR_GIB = 1 << 32


class Banks(unittest.TestCase):
    def test_the_banks_are_the_ones_reported(self) -> None:
        self.assertEqual(BDINFO['banks'], [(0x80000000, 0x7FD00000), (0x100000000, 0x80000000)])

    def test_the_total_is_four_gib_less_three_mib(self) -> None:
        total = sum(size for _, size in BDINFO['banks'])
        self.assertEqual(total, 0xFFD00000)
        self.assertEqual(total, 4093 * MIB)
        self.assertEqual(4 * 1024 * MIB - total, 3 * MIB)

    def test_the_gap_between_the_banks_is_the_top_of_the_first_4_gib_mapping(self) -> None:
        (start0, size0), (start1, _) = BDINFO['banks']
        self.assertEqual(start0 + size0, 0xFFD00000)
        self.assertEqual(start1 - (start0 + size0), 3 * MIB)
        self.assertEqual(start1, FOUR_GIB)
        self.assertEqual(banks_end(BDINFO), 0x180000000)

    def test_the_package_agrees_with_the_first_banks_end(self) -> None:
        self.assertEqual(DRAM[1], BDINFO['banks'][0][0] + BDINFO['banks'][0][1])
        secure = next(r for r in NATIVE_FULL_RAM_EXCLUSIONS if r[0] == 'secure-low-tail')
        self.assertEqual((secure[1], secure[1] + secure[2]), (0xFFD00000, FOUR_GIB))


class ExcludedTop(unittest.TestCase):
    def test_the_three_mib_are_the_secure_carveouts_below_4_gib(self) -> None:
        found = {r.name: (r.start, r.end) for r in carveouts(MC)}
        self.assertEqual(
            list(found.values()),
            [(0xFFD00000, 0xFFD40000), (0xFFD40000, 0xFFE00000), (0xFFE00000, 0xFFF00000), (0xFFF00000, FOUR_GIB)],
        )

    def test_they_run_without_a_gap_from_the_first_banks_end_to_4_gib(self) -> None:
        listed = sorted((r.start, r.end) for r in carveouts(MC))
        self.assertEqual(listed[0][0], BDINFO['banks'][0][0] + BDINFO['banks'][0][1])
        for (_, end), (start, _) in pairwise(listed):
            self.assertEqual(end, start)
        self.assertEqual(listed[-1][1], FOUR_GIB)


class UBoot(unittest.TestCase):
    def test_u_boot_sits_at_the_top_of_the_first_bank(self) -> None:
        self.assertLess(BDINFO['relocaddr'], BDINFO['TLB addr'])
        self.assertLessEqual(BDINFO['TLB addr'] + 0x10000, BDINFO['banks'][0][0] + BDINFO['banks'][0][1])
        self.assertEqual(BDINFO['relocaddr'] - BDINFO['reloc off'], UBOOT_LINK)

    def test_the_stack_and_the_fdt_are_far_below_the_image(self) -> None:
        self.assertEqual(BDINFO['sp start'], BDINFO['irq_sp'])
        self.assertEqual(BDINFO['fdt_blob'] - BDINFO['sp start'], 0x18)
        self.assertGreater(BDINFO['relocaddr'] - BDINFO['fdt_blob'], 4 * MIB)


class Loader(unittest.TestCase):
    def test_the_d0_loader_is_inside_the_first_bank_and_within_the_reviewed_end(self) -> None:
        start, size = BDINFO['banks'][0]
        self.assertTrue(start <= LOADER_SPAN[0] and LOADER_SPAN[1] <= start + size)
        self.assertLessEqual(LOADER_SPAN[1], MAX_IMAGE_END)

    def test_it_touches_no_region_the_package_or_hekate_is_known_to_use(self) -> None:
        self.assertEqual(overlaps(LOADER_SPAN, regions(BDINFO, MC)), [])

    def test_the_checker_finds_an_overlap_when_there_is_one(self) -> None:
        listed = regions(BDINFO, MC)
        self.assertEqual([r.name for r in overlaps((0xC0000000, 0xC0100000), listed)], [])
        self.assertTrue(overlaps((BDINFO['relocaddr'], BDINFO['relocaddr'] + 1), listed))
        self.assertTrue(overlaps((0xAA000000, 0xAA000010), listed))

    def test_u_boots_stack_would_need_over_a_gib_to_reach_it(self) -> None:
        self.assertGreater(BDINFO['sp start'] - LOADER_SPAN[1], 1 << 30)

    def test_the_framebuffer_is_in_the_first_bank_and_clear_of_u_boot(self) -> None:
        self.assertLessEqual(FRAMEBUFFER[1], BDINFO['sp start'])
        self.assertLessEqual(FRAMEBUFFER[1], BDINFO['banks'][0][0] + BDINFO['banks'][0][1])


if __name__ == '__main__':
    unittest.main()
