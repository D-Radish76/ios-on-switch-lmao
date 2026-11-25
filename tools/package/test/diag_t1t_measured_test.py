"""T1-T first hardware run: the recorded words, their agreement with the host fixture and the decodings the report uses.

The rate figures are the Linux clock model applied to measured words. The module clock in them is unverified.
"""

import re
import unittest
from pathlib import Path

FIXTURE = Path('boot/diag/erista-t1t-run1.txt')
CPP = Path('boot/diag/loader/test/t1t_measured_test.cpp')
PLLP_HZ = 408_000_000  # Linux's fixed rate for the PLLP_BASE row M 8, N 85, P 1
DIVISOR_T1V3 = 0xFF


def fixture() -> dict[str, int]:
    words = {}
    for line in FIXTURE.read_text().splitlines():
        if line and not line.startswith('#'):
            name, value = line.split('=')
            words[name] = int(value, 16)
    return words


def scl_hz(module_hz: float, tlow: int, thigh: int, divisor: int) -> float:
    return module_hz / ((tlow + thigh + 2) * (divisor + 1))


class Record(unittest.TestCase):
    def test_it_holds_the_twenty_five_words(self) -> None:
        self.assertEqual(25, len(fixture()))

    def test_the_host_fixture_table_equals_the_record(self) -> None:
        table = {m[1]: int(m[2], 16) for m in re.finditer(r'\{"([A-Z0-9_]+)", 0x([0-9A-Fa-f]{8})\}', CPP.read_text())}
        self.assertEqual(fixture(), table)

    def test_both_controllers_have_the_clock_on_and_are_out_of_reset(self) -> None:
        w = fixture()
        self.assertTrue(w['ENB_L'] & 1 << 12)
        self.assertFalse(w['RST_L'] & 1 << 12)
        self.assertTrue(w['ENB_H'] & 1 << 15)
        self.assertFalse(w['RST_H'] & 1 << 15)

    def test_the_timing_words_are_equal_on_both_controllers(self) -> None:
        w = fixture()
        self.assertEqual(w['I2C1_TIMING_0'], w['I2C5_TIMING_0'])
        self.assertEqual(w['I2C1_TIMING_1'], w['I2C5_TIMING_1'])

    def test_the_timing_fields(self) -> None:
        w = fixture()
        self.assertEqual((4, 2), (w['I2C1_TIMING_0'] & 0x3F, w['I2C1_TIMING_0'] >> 8 & 0x3F))
        t1 = w['I2C1_TIMING_1']
        self.assertEqual((4, 4, 7, 4), (t1 & 0x3F, t1 >> 8 & 0x3F, t1 >> 16 & 0x3F, t1 >> 24 & 0x3F))
        self.assertEqual(0, w['I2C1_TIMING_0'] & ~0x3F3F)
        self.assertEqual(0, t1 & ~0x3F3F3F3F)

    def test_timing_0_is_the_value_linux_programs_for_t210(self) -> None:
        # Linux tegra210_i2c_hw: tlow 4, thigh 2, written as THIGH << 8 | TLOW.
        self.assertEqual(2 << 8 | 4, fixture()['I2C1_TIMING_0'])

    def test_the_pllp_fields(self) -> None:
        base = fixture()['PLLP_BASE']
        self.assertEqual((8, 85, 1), (base & 0xFF, base >> 10 & 0xFF, base >> 20 & 0x1F))

    def test_the_source_fields(self) -> None:
        w = fixture()
        self.assertEqual([3, 0, 0, 0, 3, 0], [w[f'SRC_I2C{n}'] for n in range(1, 7)])
        self.assertTrue(all(w[f'SRC_I2C{n}'] >> 30 == 0 for n in range(1, 7)))


class Decoding(unittest.TestCase):
    def test_i2c1_cnfg_is_the_last_normal_mode_read_of_hekates_driver(self) -> None:
        cnfg = fixture()['I2C1_CNFG']
        self.assertEqual(0x2842, cnfg)
        self.assertEqual(2, cnfg >> 12 & 7)  # DEBOUNCE_CNT_4T
        self.assertTrue(cnfg & 1 << 11)  # NEW_MASTER_FSM
        self.assertFalse(cnfg & 1 << 10)  # packet mode off
        self.assertFalse(cnfg & 1 << 9)  # NORMAL_MODE_GO clear
        self.assertTrue(cnfg & 1 << 6)  # CMD1_READ
        self.assertEqual(1, cnfg >> 1 & 7)  # (size - 1), two bytes

    def test_i2c5_cnfg_is_packet_mode_without_debounce_which_hekates_driver_does_not_leave(self) -> None:
        cnfg = fixture()['I2C5_CNFG']
        self.assertEqual(0xC00, cnfg)
        self.assertTrue(cnfg & 1 << 10 and cnfg & 1 << 11)
        self.assertEqual(0, cnfg >> 12 & 7)  # hekate's every CNFG write sets 4T debounce

    def test_i2c5_is_busy_and_has_three_interrupt_bits_pending_and_no_error(self) -> None:
        w = fixture()
        self.assertTrue(w['I2C5_STATUS'] & 1 << 8)
        self.assertEqual(0, w['I2C5_STATUS'] & 0xF)  # NOACK field
        status = w['I2C5_INT_STATUS']
        self.assertEqual({1, 6, 7}, {bit for bit in range(32) if status >> bit & 1})
        self.assertEqual(0, status & (1 << 2 | 1 << 3))  # arbitration lost, no ack
        self.assertEqual(0, fixture()['I2C1_STATUS'] | fixture()['I2C1_INT_STATUS'])

    def test_the_divisor_words(self) -> None:
        w = fixture()
        self.assertEqual((5, 1), (w['I2C1_CLK_DIVISOR'] >> 16, w['I2C1_CLK_DIVISOR'] & 0xFFFF))
        self.assertEqual((0x19, 1), (w['I2C5_CLK_DIVISOR'] >> 16, w['I2C5_CLK_DIVISOR'] & 0xFFFF))

    def test_config_load_words(self) -> None:
        w = fixture()
        self.assertEqual(0b100, w['I2C1_CONFIG_LOAD'])
        self.assertEqual(0, w['I2C5_CONFIG_LOAD'])


class RateModel(unittest.TestCase):
    """SCL = module clock / ((TLOW + THIGH + 2) x (divisor + 1)). Module clock = 408 MHz / (source divider + 1)."""

    def module_hz(self, source: int) -> float:
        return PLLP_HZ / ((source & 0xFFFF) + 1)

    def test_the_module_clock_model(self) -> None:
        self.assertEqual(102_000_000, self.module_hz(fixture()['SRC_I2C1']))

    def test_i2c1_now(self) -> None:
        w = fixture()
        rate = scl_hz(self.module_hz(w['SRC_I2C1']), 4, 2, w['I2C1_CLK_DIVISOR'] >> 16)
        self.assertEqual(2_125_000, rate)

    def test_what_hekate_meant(self) -> None:
        # Source C0000003 is clk_m / 4: 19.2 MHz / 4 = 4.8 MHz, and the same divisor 5 gives hekate's 100 kHz.
        self.assertEqual(100_000, scl_hz(19_200_000 / 4, 4, 2, 5))

    def test_the_slowdown_by_the_t1v3_divisor_does_not_depend_on_the_module_clock(self) -> None:
        self.assertAlmostEqual(256 / 6, (DIVISOR_T1V3 + 1) / (5 + 1))

    def test_i2c1_under_t1v3(self) -> None:
        rate = scl_hz(self.module_hz(fixture()['SRC_I2C1']), 4, 2, DIVISOR_T1V3)
        self.assertAlmostEqual(49_804.6875, rate, places=3)

    def test_i2c5_now(self) -> None:
        w = fixture()
        rate = scl_hz(self.module_hz(w['SRC_I2C5']), 4, 2, w['I2C5_CLK_DIVISOR'] >> 16)
        self.assertAlmostEqual(490_384.615, rate, places=2)

    def test_the_divisors_that_would_give_about_100_khz_at_the_model_clock(self) -> None:
        module = self.module_hz(fixture()['SRC_I2C1'])
        self.assertAlmostEqual(100_393.7, scl_hz(module, 4, 2, 126), places=1)
        self.assertAlmostEqual(99_609.4, scl_hz(module, 4, 2, 127), places=1)

    def test_linuxs_multiplier_for_t210_matches_the_i2c5_divisor(self) -> None:
        # Linux tegra210: (tlow + thigh + 2) * (0x19 + 1) = 208, standard and fast mode alike.
        self.assertEqual(208, (4 + 2 + 2) * (0x19 + 1))
        self.assertEqual(0x19, fixture()['I2C5_CLK_DIVISOR'] >> 16)
