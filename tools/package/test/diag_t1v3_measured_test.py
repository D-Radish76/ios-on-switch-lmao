"""T1 v3 first hardware run: the recorded words, the host replay and what the times can and cannot show.

The rate figures are the Linux clock model applied to measured words. The module clock in them is unverified.
"""

import re
import unittest
from pathlib import Path

FIXTURE = Path('boot/diag/erista-t1v3-run1.txt')
T1T_FIXTURE = Path('boot/diag/erista-t1t-run1.txt')
CPP = Path('boot/diag/loader/test/t1v3_hardware_test.cpp')
MODULE_HZ = 102_000_000  # PLLP 408 MHz / 4, the Linux model; unverified
TLOW, THIGH = 4, 2
DIVISOR_V3 = 0xFF
DIVISOR_HEKATE = 5


def fixture() -> dict[str, int]:
    words = {}
    for line in FIXTURE.read_text().splitlines():
        if line and not line.startswith('#'):
            name, value = line.split('=')
            words[name] = int(value) if name.endswith('_US') else int(value, 16)
    return words


def scl_hz(divisor: int) -> float:
    return MODULE_HZ / ((TLOW + THIGH + 2) * (divisor + 1))


class Record(unittest.TestCase):
    def test_it_holds_the_recorded_values(self) -> None:
        self.assertEqual(29, len(fixture()))

    def test_every_hex_word_of_the_record_is_in_the_replay_text(self) -> None:
        text = CPP.read_text().upper()
        words = {v for k, v in fixture().items() if not k.endswith('_US') and k not in ('ID', 'DATA1_BEFORE_READ')}
        for word in words:
            self.assertIn(f'{word:08X}', text.replace('0X', '0x').upper())

    def test_the_replay_times_are_the_recorded_times(self) -> None:
        w = fixture()
        text = CPP.read_text()
        self.assertIn(f'busy_at[2] = {{{w["WRITE_BUSY_AT_US"]}, {w["READ_BUSY_AT_US"]}}}', text)
        self.assertEqual(w['WRITE_BUSY_CLEAR_US'], w['READ_BUSY_CLEAR_US'])
        self.assertIn(f'CLEAR AT {w["WRITE_BUSY_CLEAR_US"]} US', text)

    def test_the_before_words_repeat_t1t(self) -> None:
        t1t = {
            m[1]: int(m[2], 16)
            for m in re.finditer(r'^([A-Z0-9_]+)=([0-9A-F]{8})$', T1T_FIXTURE.read_text(), re.MULTILINE)
        }
        w = fixture()
        for mine, theirs in (
            ('RST_L', 'RST_L'),
            ('ENB_L', 'ENB_L'),
            ('SRC_I2C1', 'SRC_I2C1'),
            ('PLLP_BASE', 'PLLP_BASE'),
            ('BEFORE_CNFG', 'I2C1_CNFG'),
            ('BEFORE_STATUS', 'I2C1_STATUS'),
            ('BEFORE_INT', 'I2C1_INT_STATUS'),
            ('BEFORE_DIV', 'I2C1_CLK_DIVISOR'),
            ('BEFORE_LOAD', 'I2C1_CONFIG_LOAD'),
            ('TIMING_0', 'I2C1_TIMING_0'),
            ('TIMING_1', 'I2C1_TIMING_1'),
        ):
            self.assertEqual(t1t[theirs], w[mine], mine)


class Result(unittest.TestCase):
    def test_the_run_changed_the_divisor_and_read_the_id(self) -> None:
        w = fixture()
        self.assertEqual(0x00050001, w['BEFORE_DIV'])
        self.assertEqual(0x00FF0001, w['AFTER_DIV'])
        self.assertEqual(0x55, w['ID'])
        self.assertEqual(0xFE, w['DATA1_BEFORE_READ'])

    def test_bit_2_of_config_load_stayed_set_after_the_apply_and_both_transfer_loads(self) -> None:
        w = fixture()
        for key in ('BEFORE_LOAD', 'AFTER_LOAD', 'LOAD_AFTER_25_FIRST', 'LOAD_AFTER_25_SECOND'):
            self.assertEqual(4, w[key], key)

    def test_go_cleared_by_itself_in_cnfg(self) -> None:
        w = fixture()
        self.assertEqual(0x2800, w['WRITE_AFTER_GO_CNFG'])
        self.assertEqual(0x2840, w['READ_AFTER_GO_CNFG'])
        self.assertEqual(0, w['WRITE_AFTER_GO_CNFG'] & 0x200)
        self.assertEqual(0, w['READ_AFTER_GO_CNFG'] & 0x200)

    def test_the_bus_ended_idle(self) -> None:
        w = fixture()
        self.assertEqual(0, w['FINAL_STATUS'] | w['FINAL_INT'] | w['AFTER_STATUS'] | w['AFTER_INT'])


class Rate(unittest.TestCase):
    """Eighteen bit periods carry the address byte, one data byte and their ACK bits; start and stop add a few more."""

    BIT_PERIODS = 18

    def test_the_model_rate_after_the_change(self) -> None:
        self.assertAlmostEqual(49_804.7, scl_hz(DIVISOR_V3), delta=0.1)

    def test_both_transactions_took_the_same_time_as_for_the_same_number_of_bits(self) -> None:
        w = fixture()
        self.assertEqual(w['WRITE_BUSY_CLEAR_US'], w['READ_BUSY_CLEAR_US'])

    def test_the_time_per_bit_period_lies_between_the_model_rate_and_the_start_stop_overhead(self) -> None:
        clear = fixture()['WRITE_BUSY_CLEAR_US']
        model_us = 1e6 / scl_hz(DIVISOR_V3)
        self.assertLess(self.BIT_PERIODS * model_us, clear)
        self.assertGreater((self.BIT_PERIODS + 6) * model_us, clear)
        overhead_periods = clear / model_us - self.BIT_PERIODS
        self.assertAlmostEqual(3.6, overhead_periods, delta=0.1)

    def test_the_effective_rate_is_42_to_51_khz_for_18_to_22_periods(self) -> None:
        clear = fixture()['WRITE_BUSY_CLEAR_US']
        low, high = (periods / clear * 1e6 for periods in (self.BIT_PERIODS, 22))
        self.assertAlmostEqual(41_570, low, delta=100)
        self.assertAlmostEqual(50_800, high, delta=100)

    def test_a_hundred_kilohertz_or_more_does_not_fit_the_time(self) -> None:
        clear = fixture()['WRITE_BUSY_CLEAR_US']
        self.assertGreater(clear, self.BIT_PERIODS / 100_000 * 1e6 * 2)
        self.assertGreater(clear, 22 / 199_000 * 1e6 * 3)

    def test_busy_rose_within_a_fraction_of_one_clock_period(self) -> None:
        w = fixture()
        period_us = 1e6 / scl_hz(DIVISOR_V3)
        self.assertLess(w['WRITE_BUSY_AT_US'], period_us / 3)
        self.assertLess(w['READ_BUSY_AT_US'], period_us / 3)

    def test_the_slowdown_against_hekate_intent_and_against_t1_v2(self) -> None:
        self.assertAlmostEqual(256 / 6, (DIVISOR_V3 + 1) / (DIVISOR_HEKATE + 1), places=6)
        self.assertAlmostEqual(2_125_000, scl_hz(DIVISOR_HEKATE), delta=1)


if __name__ == '__main__':
    unittest.main()
