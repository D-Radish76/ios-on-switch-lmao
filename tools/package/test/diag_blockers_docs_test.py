"""The review documents written after D1-min: what they pin and what they must keep open."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
BLOCKERS = (ROOT / 'ERISTA_FIRST_XNU_BLOCKERS.md').read_text()
T2_REVIEW = (ROOT / 'ERISTA_T2_REVIEW.md').read_text()


def reg_offset(path: str, name: str) -> int:
    match = re.search(rf'\[{name}\] = \{{I2C1 \+ (0x[0-9a-f]+)', (ROOT / path).read_text())
    assert match, name
    return int(match.group(1), 16)


class Documents(unittest.TestCase):
    def test_no_em_dash_in_the_new_prose(self) -> None:
        for name in ('ERISTA_FIRST_XNU_BLOCKERS.md', 'ERISTA_T2_REVIEW.md', 'ERISTA_D1MIN_RESULT.md'):
            self.assertNotIn('—', (ROOT / name).read_text(), name)

    def test_the_holds_are_stated(self) -> None:
        for text in (BLOCKERS, T2_REVIEW):
            for phrase in ('`boot` ', 'HOLD'):
                self.assertIn(phrase, text)
        self.assertIn('No part of this document lowers the HOLD', BLOCKERS)
        self.assertIn('had no approval to run', T2_REVIEW)

    def test_the_xnu_sd_driver_is_called_unverified(self) -> None:
        self.assertIn("XNU's SD path", BLOCKERS)
        self.assertIn('unverified', BLOCKERS)

    def test_the_nine_steps_are_listed_and_d1_min_is_done(self) -> None:
        rows = re.findall(r'^\| (\d) \|', BLOCKERS, re.MULTILINE)
        self.assertEqual([str(n) for n in range(1, 10)], rows)
        self.assertIn('| 3 | D1-min | done: PASS |', BLOCKERS)


class ConfigLoadOffset(unittest.TestCase):
    def test_the_code_uses_8c_in_both_images_and_the_documents_agree(self) -> None:
        self.assertEqual(0x8C, reg_offset('boot/diag/loader/t1v3_regs.c', 'T1V3_I2C1_CONFIG_LOAD'))
        self.assertEqual(0x8C, reg_offset('boot/diag/loader/t2_regs.c', 'T2_HW_I2C1_CONFIG_LOAD'))
        self.assertEqual(0x94, reg_offset('boot/diag/loader/t2_regs.c', 'T2_HW_I2C1_TIMING_0'))
        for name in ('ERISTA_T2_DESIGN.md', 'ERISTA_T1V3_RUN.md'):
            text = (ROOT / name).read_text()
            self.assertRegex(text, r'`CONFIG_LOAD` \(?`\+8c`')
            self.assertNotRegex(text, r'`CONFIG_LOAD` \(?`\+94`')


class T2Claims(unittest.TestCase):
    def test_exactly_one_sensor_write_and_it_is_the_one_shot(self) -> None:
        bus = (ROOT / 'boot/diag/loader/t2_bus.c').read_text()
        self.assertEqual(1, len(re.findall(r'CNFG_WRITE2\)', bus)))
        self.assertIn('reg != T2_ONE_SHOT || value != 0', bus)
        self.assertIn('T2_BUS_DENIED', bus)

    def test_the_review_names_what_blocks_a_run(self) -> None:
        for phrase in ('No run procedure existed', 'ERISTA_T2_RUN.md', '--t2', 'separate written approval'):
            self.assertIn(phrase, T2_REVIEW)

    def test_the_cars_agree_between_t1v3_and_d0s_v2(self) -> None:
        t1v3 = (ROOT / 'boot/diag/erista-t1v3-run1.txt').read_text()
        v2 = (ROOT / 'boot/diag/erista-d0sv2-run1.txt').read_text()
        for key, name in (('RST_L', 'car_rst_devices_l'), ('ENB_L', 'car_clk_out_enb_l')):
            word = re.search(rf'^{key}=([0-9A-F]{{8}})', t1v3, re.MULTILINE).group(1)
            self.assertIn(f'value={word}', v2.split(name)[1].splitlines()[0])


if __name__ == '__main__':
    unittest.main()
