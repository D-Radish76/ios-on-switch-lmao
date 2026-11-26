"""The recorded D1-min run (boot/diag/erista-d1min-run1.txt) against the script, the loader's text and the review's criteria."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
TEXT = (ROOT / 'boot/diag/erista-d1min-run1.txt').read_text()
RECORD = dict(line.split('=', 1) for line in TEXT.splitlines() if '=' in line and not line.startswith(('#', 'uboot:', 'loader:')))
UBOOT = [line[len('uboot: ') :] for line in TEXT.splitlines() if line.startswith('uboot: ')]
LOADER = [line[len('loader: ') :] for line in TEXT.splitlines() if line.startswith('loader: ')]
SCRIPT = (ROOT / 'boot/diag/state/u-boot-script-d1min.txt').read_text()
DIAG = (ROOT / 'boot/diag/loader/diag.c').read_text()


class Verdict(unittest.TestCase):
    def test_the_two_crcs_are_equal_as_numbers(self) -> None:
        mac, switch = int(RECORD['mac_crc32_sector0'], 16), int(RECORD['switch_crc32_sector0'], 16)
        self.assertEqual(mac, switch)
        self.assertEqual(0xE1D4E0FF, mac)
        self.assertEqual('yes', RECORD['crc32_match'])
        printed = re.search(r'==> ([0-9a-f]+)$', next(line for line in UBOOT if line.startswith('CRC32 for')))
        self.assertEqual(mac, int(printed.group(1), 16))

    def test_the_signature_and_the_return(self) -> None:
        self.assertEqual('PASS', RECORD['result'])
        self.assertEqual('PRESENT', RECORD['signature_55aa'])
        self.assertIn('D1-MIN SIGNATURE 55 AA PRESENT', UBOOT)
        self.assertEqual('yes', RECORD['psci_system_reset_returned_to_hekate'])
        self.assertEqual('no', RECORD['manual_power_hold_needed'])

    def test_the_temperatures_and_their_difference(self) -> None:
        start, end = (float(RECORD[k].split()[0]) for k in ('start_temperature_nyx', 'end_temperature_nyx'))
        self.assertEqual(4.1, round(end - start, 1))
        self.assertEqual('+4.1 C', RECORD['temperature_difference'])
        self.assertLess(end, 40.0)


class AgainstTheScript(unittest.TestCase):
    def test_the_crc_range_is_the_one_sector_the_script_reads(self) -> None:
        self.assertIn('mmc read 0xA001F200 0 1', SCRIPT)
        self.assertIn('crc32 0xA001F200 0x200', SCRIPT)
        self.assertEqual(0xA001F200 + 0x200 - 1, 0xA001F3FF)
        self.assertIn('CRC32 for a001f200 ... a001f3ff', ' '.join(UBOOT))
        self.assertIn('MMC read: dev #1, block #0, count 1 ... 1 blocks read: OK', UBOOT)
        self.assertIn('mmc1 is current device', UBOOT)

    def test_the_reported_script_lines_are_in_the_script(self) -> None:
        for line in ('BOARD OK', 'FRAMEBUFFER OK', 'DTB COPIED AND FRAMEBUFFER RESERVED', 'LOADER WRITTEN TO 0xA0000000', 'D1-MIN SIGNATURE 55 AA PRESENT'):
            self.assertIn(line, UBOOT)
            self.assertIn(f'echoe "{line}', SCRIPT)

    def test_the_sector_line_is_a_prefix_of_the_script_echo(self) -> None:
        reported = 'D1-MIN SECTOR 0 READ TO 0xA001F200'
        self.assertIn(reported, UBOOT)
        self.assertIn(f'echoe "{reported}', SCRIPT)
        self.assertIn("Differences from the script's text", TEXT)


class AgainstTheLoader(unittest.TestCase):
    def test_every_loader_line_is_text_diag_c_draws(self) -> None:
        for fragment in ('LOAD DIAG', 'EL1 STATE PASS', 'MC REGISTERS READ', 'DIFFERENT FROM THE OLED', 'FDT WALK CODE', 'ALL RANGES FREE', 'FDT NODES', 'EXPORTER', 'SD REFERENCE', 'BOOT STAYS'):
            self.assertTrue(any(fragment in line for line in LOADER), fragment)
            self.assertIn(fragment, DIAG)
        self.assertIn('RESET IN 15 S', LOADER)
        self.assertIn('RESET IN', (ROOT / 'boot/diag/loader/exit.c').read_text())

    def test_the_boot_and_board_lines(self) -> None:
        self.assertIn('BOARD ERISTA T210B01=0 SKU=0 BOOT=0', LOADER)
        self.assertIn('BOOT STAYS 0', LOADER)
        self.assertIn('MC REGISTERS READ 25 DIFFERENT FROM THE OLED 0', LOADER)
        self.assertIn('FDT WALK CODE 0 ALL RANGES FREE', LOADER)

    def test_the_sd_reference_range_is_the_one_the_loader_checks(self) -> None:
        self.assertIn('SD REFERENCE 0xA001F000-0xA0020000 CLEAR BANK 0', LOADER)
        self.assertIn('0xa001f000', (ROOT / 'boot/diag/loader/ranges.h').read_text().lower())

    def test_the_loader_is_d0_rs_and_its_size_equals_d0_s_v1(self) -> None:
        self.assertEqual('88848', RECORD['loader_size_bytes'])
        self.assertIn('loader_size_bytes=88848', (ROOT / 'boot/diag/erista-d0s-run1.txt').read_text())


class Documents(unittest.TestCase):
    def test_the_result_names_what_it_does_not_show(self) -> None:
        result = (ROOT / 'ERISTA_D1MIN_RESULT.md').read_text()
        for phrase in ("XNU's SD driver is unverified", 'U-Boot\'s SD host driver', 'e1d4e0ff', '+4.1 C', 'PASS', 'not tamper protection'):
            self.assertIn(phrase, result)
        self.assertNotIn('—', result)

    def test_nothing_identifying_is_recorded(self) -> None:
        self.assertEqual([], [w for w in re.findall(r'\b[0-9a-fA-F]{8,}\b', TEXT) if w.lower() not in ('e1d4e0ff', 'a001f200', 'a001f3ff')])


if __name__ == '__main__':
    unittest.main()
