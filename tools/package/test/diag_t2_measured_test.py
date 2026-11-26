"""The recorded T2 run (boot/diag/erista-t2-run1.txt) against the host model, the sensor constants and T1 v3's words."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
TEXT = (ROOT / 'boot/diag/erista-t2-run1.txt').read_text()
LINES = [line for line in TEXT.splitlines() if '=' in line and not line.startswith('#')]
PAIRS = [line.split('=', 1) for line in LINES]
FACTS = {k: v for k, v in PAIRS if k.islower()}
WORDS = {k: v for k, v in PAIRS if k.isupper()}
CPP = (ROOT / 'boot/diag/loader/test/t2_measured_test.cpp').read_text()
BUS_TEST = (ROOT / 'boot/diag/loader/test/t2_bus_test.cpp').read_text()
T2_H = (ROOT / 'boot/diag/t2/t2.h').read_text()
RESULT = (ROOT / 'ERISTA_T2_RESULT.md').read_text()
T1V3 = dict(
    line.split('=', 1)
    for line in (ROOT / 'boot/diag/erista-t1v3-run1.txt').read_text().splitlines()
    if '=' in line and not line.startswith('#')
)


def enum_value(name: str) -> int:
    return int(re.search(rf'\b{name}\s*=\s*(0x[0-9a-fA-F]+|\d+)', T2_H).group(1), 0)


def word(key: str) -> int:
    return int(WORDS[key], 16)


def sixteenths(high: str, low: str) -> int:
    return word(high) * 16 + (word(low) >> 4)


class Verdict(unittest.TestCase):
    def test_the_record_is_a_pass_of_one_attempt(self) -> None:
        self.assertEqual('PASS', FACTS['result'])
        self.assertEqual('1', FACTS['attempts'])
        self.assertEqual('0000', FACTS['screen_stop_code'])
        self.assertEqual('yes', FACTS['psci_system_reset_returned_to_hekate'])
        self.assertEqual('no', FACTS['manual_power_hold_needed'])

    def test_the_temperatures_and_what_was_not_measured(self) -> None:
        start, end = (float(FACTS[k].split()[0]) for k in ('start_temperature_nyx', 'end_temperature_nyx'))
        self.assertEqual(1.1, round(end - start, 1))
        self.assertEqual('+1.1 C', FACTS['temperature_difference'])
        self.assertLess(end, 40.0)
        self.assertEqual('not measured', FACTS['temperature_during_run'])
        self.assertEqual('not recorded', FACTS['selection_to_return_seconds'])
        self.assertIn('not confirmed', FACTS['power_supply'])

    def test_the_mac_hash_is_one_sha256(self) -> None:
        self.assertRegex(FACTS['mac_loader_uboot_bin_sha256'], r'^[0-9a-f]{64}$')
        linux_flat = '7f5a91a4ca3d0330a7afab842b03cb07cc58c6fd8ed944bf660893a0f508f124'
        self.assertNotEqual(linux_flat, FACTS['mac_loader_uboot_bin_sha256'])

    def test_the_board_words_equal_the_first_run(self) -> None:
        first = (ROOT / 'boot/diag/erista-d0-run.log').read_text()
        self.assertIn('HIDREV 00022117', first)
        self.assertIn('ODM4 00000204', first)
        self.assertEqual(0x00022117, word('HIDREV'))
        self.assertEqual(0x204, word('ODM4'))
        self.assertEqual(19200000, int(WORDS['CNTFRQ']))
        self.assertEqual(0x80000000, word('HCR'))


class AgainstTheHostModel(unittest.TestCase):
    """142 + 9n writes and 2(15 + n) + 1 transfers with n status polls; t2_measured_test.cpp replays n = 2."""

    def test_the_screens_counts_are_the_models_for_two_polls(self) -> None:
        writes, transfers = int(FACTS['screen_writes']), int(FACTS['screen_transfers'])
        polls = (writes - 142) // 9
        self.assertEqual(2, polls)
        self.assertEqual(142 + 9 * polls, writes)
        reads = 15 + polls
        self.assertEqual(2 * reads + 1, transfers)
        self.assertEqual(2 + 9 * reads + 5, writes)

    def test_the_replay_asserts_the_same_counts(self) -> None:
        self.assertIn('EXPECT_EQ(result.writes, 160u)', CPP)
        self.assertIn('EXPECT_EQ(result.transfers, 35u)', CPP)
        self.assertIn('WRITES 160. TRANSFERS 35.', CPP)
        self.assertIn('142u + 9u * polls', BUS_TEST)

    def test_two_polls_fit_the_times(self) -> None:
        first_poll, minimum = enum_value('T2_FIRST_POLL_US'), enum_value('T2_MIN_US')
        busy, done = int(WORDS['BUSY_SEEN_AT_US']), int(WORDS['DONE_AT_US'])
        self.assertLess(first_poll, busy + 1)
        self.assertLess(busy, first_poll + 100)
        self.assertLessEqual(minimum, done)
        self.assertLess(done, minimum + 100)

    def test_the_constants_on_the_screen_are_the_headers(self) -> None:
        self.assertEqual(enum_value('T2_MIN_US'), int(WORDS['MIN_US']))
        self.assertEqual(enum_value('T2_BOUND_US'), int(WORDS['BOUND_US']))
        self.assertLess(int(WORDS['DONE_AT_US']), int(WORDS['BOUND_US']))

    def test_the_replay_holds_every_recorded_word(self) -> None:
        keys = (
            'RST_L',
            'ENB_L',
            'SRC_I2C1',
            'PLLP_BASE',
            'BEFORE_CNFG',
            'BEFORE_DIV',
            'TIMING_0',
            'TIMING_1',
            'AFTER_DIV',
        )
        for key in keys:
            self.assertIn(WORDS[key].upper(), CPP.upper(), key)
        for pair in (
            ('OLD_REMOTE_HIGH', 'OLD_REMOTE_LOW'),
            ('OLD_LOCAL_HIGH', 'OLD_LOCAL_LOW'),
            ('NEW_REMOTE_HIGH', 'NEW_REMOTE_LOW'),
            ('NEW_LOCAL_HIGH', 'NEW_LOCAL_LOW'),
        ):
            self.assertIn(f'{WORDS[pair[0]]}.{WORDS[pair[1]]}', CPP.upper(), pair)


class Sensor(unittest.TestCase):
    def test_the_sensor_answered_as_the_design_assumed(self) -> None:
        self.assertEqual(enum_value('T2_MANUFACTURER_TI'), word('ID'))
        self.assertEqual(enum_value('T2_CONFIG_SHUTDOWN'), word('CONFIG_BEFORE'))
        self.assertEqual(0, word('STATUS_BEFORE'))
        self.assertEqual(0, word('OFFSET_HIGH') | word('OFFSET_LOW'))

    def test_the_raw_words_decode_to_the_screens_temperatures(self) -> None:
        self.assertEqual(34.3125, sixteenths('NEW_REMOTE_HIGH', 'NEW_REMOTE_LOW') / 16)
        self.assertEqual(31.9375, sixteenths('NEW_LOCAL_HIGH', 'NEW_LOCAL_LOW') / 16)
        self.assertEqual(float(FACTS['screen_remote_celsius']), sixteenths('NEW_REMOTE_HIGH', 'NEW_REMOTE_LOW') / 16)
        self.assertEqual(float(FACTS['screen_local_celsius']), sixteenths('NEW_LOCAL_HIGH', 'NEW_LOCAL_LOW') / 16)

    def test_the_old_values_decode_to_what_this_test_derives(self) -> None:
        self.assertEqual(34.0625, sixteenths('OLD_REMOTE_HIGH', 'OLD_REMOTE_LOW') / 16)
        self.assertEqual(31.8125, sixteenths('OLD_LOCAL_HIGH', 'OLD_LOCAL_LOW') / 16)

    def test_both_values_changed_and_both_are_inside_the_standard_range(self) -> None:
        self.assertNotEqual(
            sixteenths('OLD_REMOTE_HIGH', 'OLD_REMOTE_LOW'), sixteenths('NEW_REMOTE_HIGH', 'NEW_REMOTE_LOW')
        )
        self.assertNotEqual(
            sixteenths('OLD_LOCAL_HIGH', 'OLD_LOCAL_LOW'), sixteenths('NEW_LOCAL_HIGH', 'NEW_LOCAL_LOW')
        )
        for key in ('NEW_REMOTE_HIGH', 'NEW_LOCAL_HIGH', 'OLD_REMOTE_HIGH', 'OLD_LOCAL_HIGH'):
            self.assertTrue(0 < word(key) < 127, key)

    def test_open_is_clear_and_the_sensor_is_back_in_shutdown(self) -> None:
        self.assertEqual(0, word('LAST_STATUS') & enum_value('T2_STATUS_OPEN'))
        self.assertEqual(enum_value('T2_CONFIG_SHUTDOWN'), word('CONFIG_AFTER'))
        self.assertEqual(word('CONFIG_BEFORE'), word('CONFIG_AFTER'))


class AgainstEarlierRuns(unittest.TestCase):
    def test_every_word_the_two_runs_share_is_equal(self) -> None:
        for mine, theirs in (
            ('RST_L', 'RST_L'),
            ('ENB_L', 'ENB_L'),
            ('SRC_I2C1', 'SRC_I2C1'),
            ('PLLP_BASE', 'PLLP_BASE'),
            ('BEFORE_CNFG', 'BEFORE_CNFG'),
            ('BEFORE_STATUS', 'BEFORE_STATUS'),
            ('BEFORE_INT', 'BEFORE_INT'),
            ('BEFORE_DIV', 'BEFORE_DIV'),
            ('BEFORE_LOAD', 'BEFORE_LOAD'),
            ('TIMING_0', 'TIMING_0'),
            ('TIMING_1', 'TIMING_1'),
            ('AFTER_LOAD', 'AFTER_LOAD'),
            ('AFTER_DIV', 'AFTER_DIV'),
            ('AFTER_STATUS', 'AFTER_STATUS'),
            ('AFTER_INT', 'AFTER_INT'),
        ):
            self.assertEqual(int(T1V3[theirs], 16), word(mine), mine)
        self.assertEqual(int(T1V3['ID'], 16), word('ID'))

    def test_the_two_car_words_equal_d0s_v2(self) -> None:
        d0s = (ROOT / 'boot/diag/erista-d0sv2-run1.txt').read_text()
        self.assertIn(f'value={WORDS["RST_L"]}', d0s)
        self.assertIn(f'value={WORDS["ENB_L"]}', d0s)

    def test_the_divisor_was_written_and_not_restored(self) -> None:
        self.assertEqual(0x00050001, word('BEFORE_DIV'))
        self.assertEqual(0x00FF0001, word('AFTER_DIV'))


class Result(unittest.TestCase):
    def test_the_document_states_what_the_run_does_not_cover(self) -> None:
        for sentence in (
            'does not show temperature monitoring while XNU runs',
            'does not show fan control',
            'does not give an autonomous hang protection',
            '31004 us is the time of the poll that saw BUSY clear and is not the conversion time',
        ):
            self.assertIn(sentence, RESULT)

    def test_the_document_keeps_the_holds(self) -> None:
        for sentence in ('`boot` stays 0', 'feature mask stays 0', 'T3, T4'):
            self.assertIn(sentence, RESULT)

    def test_the_document_holds_the_decoded_figures_and_the_counts(self) -> None:
        for text in ('34.3125', '31.9375', '160', '35', '142 + 9n', '2005', '31004'):
            self.assertIn(text, RESULT)


if __name__ == '__main__':
    unittest.main()
