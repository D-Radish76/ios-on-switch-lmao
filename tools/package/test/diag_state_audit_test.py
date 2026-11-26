"""The D0-S script as U-Boot will read it (rendered for the Erista): every command, every address, every store.

The final audit before D0-S (ERISTA_D0S_RUN.md). It reads the rendered text, so the substituted board, loader and DTB
values are the ones audited. Nothing is built or run.
"""

import re
import unittest
from pathlib import Path

from diag_loader import diag_script

ROOT = Path('.')
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
DTB_BYTES = len((ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes())
RENDERED = diag_script((ROOT / 'boot/diag/state/u-boot-script-state.txt').read_text(), HEADER, 'erista', DTB_BYTES)
D0 = diag_script((ROOT / 'boot/diag/loader/u-boot-script.txt').read_text(), HEADER, 'erista', DTB_BYTES)
LINES = [l.strip() for l in RENDERED.splitlines() if l.strip() and not l.strip().startswith('#')]
COMMANDS = {'base', 'bootm', 'cp.b', 'do', 'done', 'echoe', 'else', 'fdt', 'fi', 'for', 'itest.l', 'reset', 'setenv',
            'setexpr', 'setexpr.l', 'sleep', 'test', 'while'}


def commands():
    found = set()
    for line in LINES:
        for part in re.split(r'&&|;\s*then|\bthen\b|;\s*do\b', line):
            part = re.sub(r'^(if|elif)\s+', '', part.strip())
            if part:
                found.add(part.split(' ')[0])
    return found


class D0SAudit(unittest.TestCase):
    def test_only_these_commands_appear(self):
        self.assertLessEqual(commands(), COMMANDS | {'if'})

    def test_the_device_reads_are_the_four_and_no_others(self):
        reads = re.findall(r'setexpr\.l (\w+) \*(0x[0-9a-fA-F]+)', RENDERED)
        self.assertEqual(sorted(a for _, a in reads), ['0x60006004', '0x60006010', '0x700b0024', '0x700b002c'])
        for _, address in reads:
            value = int(address, 16)
            self.assertEqual(value % 4, 0)
            self.assertTrue(0x60006000 <= value < 0x60007000 or 0x700B0000 <= value < 0x700B0200, address)
        self.assertEqual(sorted(set(re.findall(r'itest\.\w \*(0x[0-9a-fA-F]+)', RENDERED))), ['0x8D000000', '0x8D000004'])

    def test_every_store_is_ram_that_d0_already_writes(self):
        stores = [line for line in LINES if re.search(r'\bcp\.b\b|\bfdt rsvmem\b|\bbootm (start|loados)\b', line)]
        self.assertEqual(len(stores), 4)
        self.assertRegex(stores[0], r'cp\.b 0x[0-9a-f]+ 0x8D000000 0x[0-9a-f]+')
        self.assertIn('fdt rsvmem add 0xF5A00000 0x400000', stores[1])
        self.assertRegex(stores[2], r'bootm start 0x[0-9a-f]+ - 0x8D000000')
        self.assertIn('bootm loados', stores[3])
        for line in LINES:
            if re.search(r'\bcp\.b\b|\bfdt rsvmem\b|\bbootm (start|loados)\b', line):
                self.assertIn(line, [l.strip() for l in D0.splitlines()])

    def test_the_sdmmc1_reads_wait_for_both_car_tests(self):
        read = RENDERED.index('*0x700b0024')
        gate = RENDERED.rindex('if test', 0, read)
        self.assertIn('"${state_sd_reset}" = "0" -a "${state_sd_clock}" = "4000"', RENDERED[gate:read])
        self.assertIn('\\& 0x4000', RENDERED[:gate])
        self.assertEqual(RENDERED.count('\\& 0x4000'), 2)

    def test_the_script_ends_in_one_reset_and_hands_off_once(self):
        self.assertEqual(sum(1 for l in LINES if l == 'reset'), 1)
        self.assertEqual(LINES[-4:], ['reset', 'while true; do', 'sleep 1', 'done'])
        self.assertEqual(sum(1 for l in LINES if l == 'bootm go'), 1)

    def test_no_command_reaches_the_card_the_network_or_the_environment_store(self):
        text = '\n'.join(LINES)
        self.assertNotRegex(text, r'\b(mmc|usb|sf|nand|fatload|ext4load|load|tftp|dhcp|saveenv|env save|run|source|go|mw|mm|nm|i2c)\b ')

    def test_the_failure_statuses_are_the_ones_the_run_sheet_lists(self):
        statuses = set(re.findall(r'setenv handoff_status ([A-Z_]+)', RENDERED)) - {'READY'}
        run_sheet = (ROOT / 'ERISTA_D0S_RUN.md').read_text()
        for status in statuses:
            self.assertIn(status, run_sheet, status)


if __name__ == '__main__':
    unittest.main()
