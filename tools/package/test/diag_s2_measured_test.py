"""The recorded D0-S v2 run (boot/diag/erista-d0sv2-run1.txt) against the loader's own text and logic.

Only what the user reported is recorded. The register values were not reported, so the record must say so and carry no number
for them.
"""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
RECORD = {
    key: value
    for key, _, value in (
        line.partition('=') for line in (ROOT / 'boot/diag/erista-d0sv2-run1.txt').read_text().splitlines() if '=' in line and not line.startswith('#')
    )
}  # fmt: skip
LOADER = ROOT / 'boot/diag/loader'


class RecordedRun(unittest.TestCase):
    def test_the_verdict_and_the_reported_facts(self) -> None:
        self.assertEqual('PASS', RECORD['result'])
        self.assertEqual('4', RECORD['reads'])
        self.assertEqual('0000', RECORD['stop'])
        self.assertEqual('yes', RECORD['psci_system_reset_returned_to_hekate'])
        self.assertEqual('no', RECORD['manual_power_hold_needed'])
        self.assertEqual('35 C', RECORD['end_temperature_nyx'])
        self.assertEqual('not recorded', RECORD['start_temperature_nyx'])

    def test_the_values_that_were_not_reported_are_not_written(self) -> None:
        for key in ('car_rst_devices_l', 'car_clk_out_enb_l', 'sdmmc1_present_state', 'sdmmc1_clock_control'):
            self.assertEqual('not reported', RECORD[key], key)
        text = (ROOT / 'boot/diag/erista-d0sv2-run1.txt').read_text()
        self.assertEqual([], [w for w in re.findall(r'\b[0-9A-F]{8}\b', text) if w != 'SYSTEM'])

    def test_the_complete_line_is_what_the_loader_prints(self) -> None:
        source = (LOADER / 's2_report.c').read_text()
        self.assertIn('"S2 COMPLETE. READS "', source)
        self.assertIn('". STOP "', source)
        self.assertEqual(f'S2 COMPLETE. READS {RECORD["reads"]}. STOP {RECORD["stop"]}', RECORD['screen_complete_line'])

    def test_four_reads_is_only_possible_behind_the_open_gate(self) -> None:
        source = (LOADER / 's2.c').read_text()
        run = source[source.index('void switch_s2_run(') :]
        gate = run.index('S2_STEP_GATE')
        self.assertLess(run.index('S2_CAR_CLK_OUT_ENB_L'), gate)
        self.assertGreater(run.index('S2_SDMMC1_PRESENT_STATE'), gate)
        self.assertIn('result->sd_state == SWITCH_S2_SD_READ', run)
        self.assertEqual(4, len(re.findall(r'\bstep_read\(', run)))

    def test_the_documents_keep_the_open_points_open(self) -> None:
        result = (ROOT / 'ERISTA_D0S_V2_RESULT.md').read_text()
        for phrase in ('not identified', 'not reported', 'PASS', '35 C', 'start temperature was not recorded'):
            self.assertIn(phrase, result)
        notes = (ROOT / 'NOTES.md').read_text()
        self.assertIn('D0-S v1\'s hung instruction is still not identified', notes)
        self.assertNotIn('—', result)


if __name__ == '__main__':
    unittest.main()
