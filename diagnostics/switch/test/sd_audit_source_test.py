"""The facts ERISTA_FIRST_XNU_BOOT.md's SD driver audit rests on, read from NativeSwitchBlock.cpp."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
SOURCE = (ROOT / 'drivers/NativeSwitchBlock/NativeSwitchBlock.cpp').read_text()


def body(signature):
    start = SOURCE.index(signature)
    return SOURCE[start : SOURCE.index('\n}\n', start)]


class SdAuditTest(unittest.TestCase):
    def test_speed_raise_skips_before_any_pmic_access(self):
        speed = body('bool NativeSwitchBlock::raiseBusSpeed()')
        order = [speed.index(text) for text in ('switch-no-sd-uhs', 'APB_GP_HIDREV', 'tegra_i2c_init', 'enablePllc4')]
        self.assertEqual(order, sorted(order))
        self.assertIn('>> 4) & 0xf) < 2', speed)

    def test_only_three_functions_write_the_pmic(self):
        writers = []
        for match in re.finditer(r'tegra_i2c_write\(', SOURCE):
            head = SOURCE.rfind('\nbool NativeSwitchBlock::', 0, match.start()), SOURCE.rfind('\nvoid NativeSwitchBlock::', 0, match.start())
            at = max(head)
            writers.append(SOURCE[at : SOURCE.index('(', at)].strip())
        self.assertEqual(sorted(set(writers)), ['bool NativeSwitchBlock::switchTo1V8', 'void NativeSwitchBlock::powerOffCard', 'void NativeSwitchBlock::powerOnController'])
        self.assertIn('switchTo1V8(', body('bool NativeSwitchBlock::reinitialize'))

    def test_no_partition_means_setup_fails_before_any_card_write_is_possible(self):
        setup = body('bool NativeSwitchBlock::setup()')
        self.assertIn('if (found != 1 ||', setup)
        self.assertLess(setup.index('if (found != 1 ||'), setup.index('checkCq('))
        self.assertIn('Until setup()', SOURCE)
        self.assertRegex(SOURCE, r'panic\("SwitchSD: write lba=%u n=%u outside the APFS partition')

    def test_extension_writes_are_the_cq_control_bytes_only(self):
        self.assertEqual(SOURCE.count('write ? 49 : 48'), 1)
        control = body('int NativeSwitchBlock::CqBus::control')
        self.assertIn('known', control)
        self.assertIn('panic("SwitchSD: extension register', control)

    def test_root_wait_reports_an_unpublished_device(self):
        diag = SOURCE[SOURCE.index('extern "C" void switch_raw_root_wait_diagnostic') :][:600]
        self.assertIn('not published', diag)


if __name__ == '__main__':
    unittest.main()
