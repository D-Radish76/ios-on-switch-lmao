"""The D1-min script as U-Boot will read it (rendered for the Erista): every command, address and store.

The offline review before D1-min (ERISTA_D1MIN_REVIEW.md). It reads the rendered text and the BUILD targets. Nothing is built or run.
"""

import re
import unittest
from pathlib import Path

from diag_loader import diag_script

ROOT = Path('.')
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
DTB_BYTES = len((ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes())
RENDERED = diag_script((ROOT / 'boot/diag/state/u-boot-script-d1min.txt').read_text(), HEADER, 'erista', DTB_BYTES)
D0 = diag_script((ROOT / 'boot/diag/loader/u-boot-script.txt').read_text(), HEADER, 'erista', DTB_BYTES)
LINES = [ln.strip() for ln in RENDERED.splitlines() if ln.strip() and not ln.strip().startswith('#')]
CODE = '\n'.join(LINES)
COMMANDS = {'base', 'bootm', 'cp.b', 'crc32', 'do', 'done', 'echoe', 'else', 'fdt', 'fi', 'for', 'itest.l', 'itest.w', 'mmc',
            'reset', 'setenv', 'sleep', 'test', 'while'}  # fmt: skip


def block(text: str, start: str, end: str) -> str:
    return text[text.index(start) : text.index(end)]


class D1MinScript(unittest.TestCase):
    def test_only_these_commands_appear(self) -> None:
        found = set()
        for line in LINES:
            for part in re.split(r'&&|;\s*then|\bthen\b|;\s*do\b', line):
                part = re.sub(r'^(if|elif)\s+', '', part.strip())
                if part:
                    found.add(part.split(' ')[0])
        self.assertLessEqual(found, COMMANDS | {'if'})

    def test_the_card_is_reached_by_exactly_mmc_dev_1_and_one_sector_read(self) -> None:
        self.assertEqual(['mmc dev 1', 'mmc read 0xA001F200 0 1'], re.findall(r'\bmmc [^;&\n]*?(?= *(?:;|&&|$))', CODE))
        for verb in ('write', 'erase', 'part', 'rescan', 'info', 'bootsel', 'setdsr', 'swrite', 'dev 0', 'dev 2'):
            self.assertNotIn(f'mmc {verb}', CODE)
        self.assertNotRegex(CODE, r'\b(fatwrite|ext4write|sf|nand|usb|saveenv|env save|gpt|mw|mm|nm|i2c|md|cmp)\b ')

    def test_the_read_is_one_block_from_sector_zero_into_ram_above_the_loader(self) -> None:
        self.assertIn('mmc read 0xA001F200 0 1', CODE)
        link = (ROOT / 'boot/diag/loader/link.ld').read_text()
        ceiling = int(re.search(r'__image_end <= (0x[0-9A-Fa-f]+),', link)[1], 16)
        self.assertGreaterEqual(0xA001F200, ceiling)
        self.assertIn('0xA001F200', (ROOT / 'boot/u-boot-script.txt').read_text())

    def test_the_crc_is_printed_and_the_sector_itself_never_is(self) -> None:
        self.assertEqual(['crc32 0xA001F200 0x200'], re.findall(r'^crc32 .*$', CODE, re.M))
        self.assertEqual(['itest.w *0xA001F3FE == 0xAA55'], re.findall(r'itest\.w [^;&]*?(?= *;)', CODE))
        self.assertNotRegex(CODE, r'\becho[e]? [^\n]*\$\{?sector')
        self.assertNotRegex(CODE, r'\b(md|md\.[bwl]|printenv|fatls|ls)\b ')

    def test_nothing_is_compared_with_an_expected_crc(self) -> None:
        self.assertNotRegex(CODE, r'crc32 [^\n]*0xA001F200 0x200 ')

    def test_no_load_from_a_device_address(self) -> None:
        for address in re.findall(r'\*0x[0-9a-fA-F]+', CODE):
            self.assertIn(address, ('*0x8D000000', '*0x8D000004', '*0xA001F3FE'))
        self.assertNotIn('setexpr', CODE)

    def test_the_card_block_is_after_the_loader_is_written_and_before_bootm_prep(self) -> None:
        order = [CODE.index(s) for s in ('bootm loados', 'mmc dev 1', 'mmc read', 'crc32', 'itest.w', 'bootm prep', 'bootm go')]
        self.assertEqual(sorted(order), order)

    def test_it_is_the_d0_script_plus_the_title_and_the_card_block(self) -> None:
        d0 = [ln.strip() for ln in D0.splitlines() if ln.strip() and not ln.strip().startswith('#')]
        extra = [ln for ln in LINES if ln not in d0]
        self.assertEqual(
            ['echoe "D1-MIN DIAGNOSTIC. ONE SECTOR READ. NO KERNEL. NO BOOT"', 'if mmc dev 1; then', 'if mmc read 0xA001F200 0 1; then'],
            extra[:3],
        )
        removed = [ln for ln in d0 if ln not in LINES]
        self.assertEqual(['echoe "D0 LOADER DIAGNOSTIC. NO KERNEL. NO BOOT"'], removed)

    def test_a_failed_step_names_its_status_and_the_script_resets_once(self) -> None:
        self.assertIn('setenv handoff_status SD_DEVICE_SELECT_FAILED', CODE)
        self.assertIn('setenv handoff_status SD_SECTOR_READ_FAILED', CODE)
        self.assertEqual(1, sum(1 for ln in LINES if ln == 'reset'))
        self.assertEqual(['reset', 'while true; do', 'sleep 1', 'done'], LINES[-4:])
        self.assertEqual(1, sum(1 for ln in LINES if ln == 'bootm go'))
        self.assertNotRegex(CODE, r'\b(retry|until)\b')

    def test_every_status_is_in_the_review(self) -> None:
        review = (ROOT / 'ERISTA_D1MIN_REVIEW.md').read_text()
        for status in set(re.findall(r'setenv handoff_status ([A-Z_]+)', RENDERED)) - {'READY'}:
            self.assertIn(status, review, status)


class D1MinTarget(unittest.TestCase):
    @staticmethod
    def target(name: str) -> str:
        text = (ROOT / 'boot/BUILD.bazel').read_text()
        block_text = text[text.index(f'name = "{name}"') :]
        return block_text[: block_text.index('\n)\n')]

    def test_it_builds_d0_rs_loader_and_only_the_script_and_entry_differ(self) -> None:
        d1, d0r = self.target('diag_d1min'), self.target('diag_loader_r')
        for key in ('srcs = [', 'hdrs = [', 'defines = ['):
            def part(text: str, key: str = key) -> str:
                return text[text.index(key) :].split(']', 1)[0]

            self.assertEqual(part(d0r), part(d1), key)
        for line in ('linker_script = ":diag/loader/link.ld"', 'board = "erista"', 'bl33 = "//bazel/pinned:firmware/bl33.bin"'):
            self.assertIn(line, d1)
            self.assertIn(line, d0r)
        self.assertIn('entry = "ios-diag-d1min"', d1)
        self.assertIn('script_template = ":diag/state/u-boot-script-d1min.txt"', d1)
        for flag in ('t1 = True', 't1r = True', 't1t = True', 't1v3 = True', 't2 = True', 's2 = True'):
            self.assertNotIn(flag, d1)

    def test_the_d0_r_hash_is_recorded(self) -> None:
        recorded = (ROOT / 'boot/diag/erista-diag-image-hashes.txt').read_text()
        self.assertIn('diag_loader_r c101d9c3ed21b8d62f1370e33871b857a41ffff64f8214092cece1d151b8105e', recorded)


class PassCriteria(unittest.TestCase):
    def test_the_review_sets_the_pass_conditions_and_the_start_temperature(self) -> None:
        review = (ROOT / 'ERISTA_D1MIN_REVIEW.md').read_text()
        for phrase in ('equals the CRC32 of the card\'s sector 0 taken on the Mac beforehand', '`55 AA PRESENT` for this card', "Nyx's start temperature"):
            self.assertIn(phrase, review)

    def test_next_diagnostics_names_u_boot_as_the_printer(self) -> None:
        text = ' '.join((ROOT / 'ERISTA_NEXT_DIAGNOSTICS.md').read_text().split())
        self.assertIn("U-Boot's script prints the CRC32", text)
        self.assertNotIn('the loader prints only the CRC32', text)


if __name__ == '__main__':
    unittest.main()
