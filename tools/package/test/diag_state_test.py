"""D0-S (state) and D1-min: the D0 script plus one read-only block each, over a loader that is D0-R's, byte for byte.

The two scripts are read as text and rendered as the D0 test renders its own. Nothing is built here: the Mac compares the
three loader images.
"""

import re
import unittest
from pathlib import Path

from diag_loader import diag_script

ROOT = Path('.')
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
DTB_BYTES = len((ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes())
D0 = (ROOT / 'boot/diag/loader/u-boot-script.txt').read_text()
STATE = (ROOT / 'boot/diag/state/u-boot-script-state.txt').read_text()
D1MIN = (ROOT / 'boot/diag/state/u-boot-script-d1min.txt').read_text()
BUILD = (ROOT / 'boot/BUILD.bazel').read_text()
LINK = (ROOT / 'boot/diag/loader/link.ld').read_text()


def code(text):
    return [line for line in text.splitlines() if not line.lstrip().startswith('#')]


def without(text, block):
    return '\n'.join(line for line in code(text) if line not in block)


def target(name):
    start = BUILD.index(f'diag_loader_package(\n    name = "{name}"')
    return BUILD[start : BUILD.index('\n)\n', start)]


class Shape(unittest.TestCase):
    def test_each_is_the_d0_script_plus_one_block(self):
        for text, banner in ((STATE, 'D0-S STATE'), (D1MIN, 'D1-MIN DIAGNOSTIC')):
            added = [line for line in code(text) if line not in code(D0)]
            removed = [line for line in code(D0) if line not in code(text)]
            self.assertEqual(removed, ['    echoe "D0 LOADER DIAGNOSTIC. NO KERNEL. NO BOOT"'])
            self.assertIn(banner, added[0])
            self.assertEqual(without(text, added[1:] + [added[0]]).count('bootm'), without(D0, []).count('bootm'))

    def test_both_render_with_the_d0_values(self):
        for text in (STATE, D1MIN):
            rendered = diag_script(text, HEADER, 'erista', DTB_BYTES)
            self.assertNotRegex(rendered, r'@[A-Z_0-9]+@')
            self.assertIn('"${t210b01}" = "0" -a "${sku}" = "0"', rendered)

    def test_the_blocks_sit_where_the_header_says(self):
        self.assertLess(STATE.index('if bootm prep'), STATE.index('D0-S FRAMEBUFFER'))
        self.assertLess(STATE.index('D0-S SDMMC1 PRESENT'), STATE.index('        bootm go'))
        self.assertLess(D1MIN.index('if bootm loados'), D1MIN.index('if mmc dev 1'))
        self.assertLess(D1MIN.index('if mmc read'), D1MIN.index('if bootm prep'))


class State(unittest.TestCase):
    def test_every_device_read_is_listed(self):
        reads = set(re.findall(r'setexpr\.l \w+ \*(0x[0-9a-f]+)', STATE))
        self.assertEqual(reads, {'0x60006004', '0x60006010', '0x700b0024', '0x700b002c'})

    def test_sdmmc1_is_read_only_with_its_clock_on_and_out_of_reset(self):
        read = STATE.index('*0x700b0024')
        gate = STATE.rindex('if test', 0, read)
        self.assertIn('"${state_sd_reset}" = "0" -a "${state_sd_clock}" = "4000"', STATE[gate:read])
        self.assertEqual(STATE.count('0x700b'), 2)

    def test_no_store_to_a_device_the_card_or_flash(self):
        text = '\n'.join(code(STATE))
        self.assertNotRegex(text, r'\bmw(\.[bwlq])?\b|\bmm\b|\bnm\b|\bmmc\b|\bfatwrite\b|\bsaveenv\b|\bgo\s+0x')
        self.assertEqual(len(re.findall(r'\bsetexpr(?:\.l)? (?:\w+) \*', text)), 4)

    def test_the_bit_test_fails_safe(self):
        self.assertIn('D0-S CAR BIT TEST FAILED: SDMMC1 NOT READ', STATE)
        self.assertIn('D0-S SDMMC1 CLOCK OFF OR IN RESET: NOT READ', STATE)


class D1Min(unittest.TestCase):
    def test_one_device_one_sector_no_write(self):
        text = '\n'.join(code(D1MIN))
        self.assertEqual(re.findall(r'\bmmc [a-z]+[^;\n]*', text), ['mmc dev 1', 'mmc read 0xA001F200 0 1'])
        self.assertNotRegex(text, r'\bmmc (write|erase|part|rescan)\b|\bfatload\b|\bload mmc\b|\bexporter\b|\bgo\s+0x')
        self.assertNotRegex(text, r'\bmw(\.[bwlq])?\b|\bmd(\.[bwlq])?\b|\bsetexpr')

    def test_only_the_crc_and_the_signature_are_shown(self):
        text = '\n'.join(code(D1MIN))
        self.assertEqual(re.findall(r'\bcrc32 [^\n]*', text), ['crc32 0xA001F200 0x200'])
        self.assertEqual(re.findall(r'itest\.w \*0x[0-9A-Fa-f]+ == 0x[0-9A-Fa-f]+', text), ['itest.w *0xA001F3FE == 0xAA55'])
        self.assertNotRegex(text, r'\*0xA001F2[0-9A-Fa-f]{2}\b(?!.*0xA001F3FE)')

    def test_the_sector_lands_above_the_loader_and_its_stamps(self):
        ends = int(re.search(r'0xa001ef00', LINK, re.I).group(0), 16)
        self.assertLessEqual(ends, 0xA001F200)
        self.assertEqual(0xA001F200 + 0x200, 0xA001F400)
        self.assertLess(0xA001F400, 0xA0020000)

    def test_a_failed_step_refuses_like_d0(self):
        self.assertIn('setenv handoff_status SD_SECTOR_READ_FAILED', D1MIN)
        self.assertIn('setenv handoff_status SD_DEVICE_SELECT_FAILED', D1MIN)


class Images(unittest.TestCase):
    def test_both_targets_are_d0rs_loader_with_another_script_and_entry(self):
        reference = target('diag_loader_r')
        for name, entry in (('diag_state', 'ios-diag-state'), ('diag_d1min', 'ios-diag-d1min')):
            own = target(name)
            changed = {'name', 'entry', 'ident', 'title', 'script_template'}
            def rows(text):
                return {m.group(1): m.group(2) for m in re.finditer(r'^    (\w+) = (.*)$', text, re.M)}
            self.assertEqual({k for k, v in rows(own).items() if rows(reference).get(k) != v}, changed)
            self.assertIn(f'entry = "{entry}"', own)
            self.assertEqual(own.split('srcs = [')[1].split('],')[0], reference.split('srcs = [')[1].split('],')[0])
            self.assertIn('defines = ["-DSWITCH_DIAG_RESERVATIONS=1"]', own)

    def test_the_other_images_still_use_their_own_scripts(self):
        self.assertEqual(target('diag_loader_r').count('script_template = ":diag/loader/u-boot-script.txt"'), 1)
        self.assertEqual(target('diag_loader_sd').count('diag/sd_loader/u-boot-script.txt'), 1)


if __name__ == '__main__':
    unittest.main()
