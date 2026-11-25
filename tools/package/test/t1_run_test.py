"""The first T1 v2 run on the Erista (boot/diag/erista-t1-run.log): what it recorded, and what decoding it gives."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
TEXT = (ROOT / 'boot/diag/erista-t1-run.log').read_text()
BODY = '\n'.join(line for line in TEXT.splitlines() if not line.startswith('#'))

CLK_L_I2C1 = 1 << 12
CLK_L_PWM = 1 << 17
HEKATE_I2C1_SOURCE = (6 << 29) | 3
HEKATE_PWM_SOURCE = (6 << 29) | 4


def word(label: str) -> int:
    m = re.search(rf'{label} ([0-9A-F]{{8}})', BODY)
    assert m, label
    return int(m.group(1), 16)


class Recorded(unittest.TestCase):
    def test_the_board_is_an_erista_with_boot_zero(self) -> None:
        self.assertIn('BOARD ERISTA T210B01=0 SKU=0 BOOT=0', BODY)
        self.assertIn('EL1 STATE PASS', BODY)
        self.assertIn('BOOT STAYS 0.', BODY)

    def test_it_stopped_at_the_first_transfer_with_no_ack(self) -> None:
        self.assertIn('T1 STOPPED CODE 0507. NO ACK FROM 0X4C', BODY)
        self.assertIn('TRANSFERS 0 OF 10', BODY)
        self.assertIn('NO RETRY. NO SENSOR WRITE.', BODY)
        self.assertIn('FAN AND PWM UNTOUCHED.', BODY)

    def test_status_after_is_not_busy_and_has_noack_bit_zero_only(self) -> None:
        after = word('STATUS AFTER')
        self.assertEqual(after & 0x100, 0)
        self.assertEqual(after & 0xF, 1)
        self.assertEqual(after & ~0x10F, 0)

    def test_nothing_that_looks_like_a_temperature_or_a_card_identity_is_in_the_log(self) -> None:
        self.assertNotRegex(BODY, r'DEG|CELSIUS|\d\s*°')
        self.assertNotIn('CID', BODY)


class Decoded(unittest.TestCase):
    def test_the_i2c1_and_pwm_clocks_are_on_and_out_of_reset(self) -> None:
        for bit in (CLK_L_I2C1, CLK_L_PWM):
            self.assertTrue(word('ENB_L') & bit)
            self.assertFalse(word('RST_L') & bit)

    def test_the_i2c1_divisor_is_hekates_but_its_source_field_is_not(self) -> None:
        source = word('SRC_I2C1')
        self.assertEqual(source & 0xFFFF, HEKATE_I2C1_SOURCE & 0xFFFF)
        self.assertEqual(source >> 29, 0)
        self.assertNotEqual(source, HEKATE_I2C1_SOURCE)

    def test_the_pwm_source_is_exactly_hekates_clock_enable_pwm_value(self) -> None:
        self.assertEqual(word('SRC_PWM'), HEKATE_PWM_SOURCE)

    def test_the_controller_divisor_is_hekates_and_the_pads_are_input_enable_only(self) -> None:
        self.assertEqual(word('DIV'), 0x00050001)
        self.assertEqual(word('SCL'), 0x40)
        self.assertEqual(word('SDA'), 0x40)

    def test_cnfg_is_left_over_from_a_two_byte_normal_mode_read_with_no_go_bit(self) -> None:
        cnfg = word('CNFG')
        self.assertEqual(cnfg & 0x200, 0)
        self.assertEqual(cnfg & 0x800, 0x800)
        self.assertEqual(cnfg & 0x40, 0x40)
        self.assertEqual((cnfg >> 1) & 7, 1)
        self.assertEqual(cnfg >> 12, 2)

    def test_config_load_reads_back_with_only_the_timeout_bit(self) -> None:
        self.assertEqual(word('LOAD'), 0x4)

    def test_the_bus_frequency_follows_hekates_formula_for_either_source(self) -> None:
        def scl(source_hz: int, divisor_field: int) -> float:
            return source_hz / (divisor_field + 1) / (4 + 2 + 2) / (((word('DIV') >> 16) & 0xFFFF) + 1)

        self.assertAlmostEqual(scl(19_200_000, 3), 100_000)
        self.assertAlmostEqual(scl(408_000_000, 3), 2_125_000)


if __name__ == '__main__':
    unittest.main()
