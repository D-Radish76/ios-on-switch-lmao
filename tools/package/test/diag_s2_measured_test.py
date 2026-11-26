"""The recorded D0-S v2 run (boot/diag/erista-d0sv2-run1.txt) against the loader's own text and logic.

The four register values, read by the user from the photograph, are checked against the loader's allowlist (address and width) and
against the gate the loader applies.
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

    def test_the_four_registers_have_the_allowlisted_address_and_width(self) -> None:
        allow = (LOADER / 's2_regs.c').read_text().lower()
        expected = {
            'car_rst_devices_l': ('0x60006004', 32, '049102CA'),
            'car_clk_out_enb_l': ('0x60006010', 32, '9802D131'),
            'sdmmc1_present_state': ('0x700B0024', 32, '01FB0000'),
            'sdmmc1_clock_control': ('0x700B002C', 16, '0007'),
        }
        for key, (address, width, value) in expected.items():
            self.assertEqual(f'{address} width={width} value={value}', RECORD[key], key)
            self.assertIn(f'{{{address.lower()}u, s2_width_{width}}}', allow, key)

    def test_the_values_agree_with_the_gate(self) -> None:
        reset = int(RECORD['car_rst_devices_l'].split('value=')[1], 16)
        clock = int(RECORD['car_clk_out_enb_l'].split('value=')[1], 16)
        self.assertEqual(0, reset >> 14 & 1)
        self.assertEqual(1, clock >> 14 & 1)

    def test_present_state_and_clock_control_fields(self) -> None:
        state = int(RECORD['sdmmc1_present_state'].split('value=')[1], 16)
        control = int(RECORD['sdmmc1_clock_control'].split('value=')[1], 16)
        self.assertEqual(0b11, state >> 16 & 0b11)
        self.assertEqual(0, state >> 18 & 1)
        self.assertEqual(1, state >> 19 & 1)
        self.assertEqual(0xF, state >> 20 & 0xF)
        self.assertEqual(1, state >> 24 & 1)
        self.assertEqual(0, state & 0b11)
        self.assertEqual(0b111, control & 0b111)
        self.assertEqual(0, control >> 3)

    def test_the_framebuffer_matches_the_script_and_the_v1_record(self) -> None:
        script = (ROOT / 'boot/diag/state/u-boot-script-state-v2.txt').read_text()
        self.assertEqual('0xF5A00000', RECORD['framebuffer_address'])
        self.assertEqual((720, 1280), (int(RECORD['framebuffer_width']), int(RECORD['framebuffer_height'])))
        self.assertEqual('3', RECORD['framebuffer_rotation'])
        for text in ('== 0xF5A00000', '== 0x2D0', '== 0x500', '"a8b8g8r8"', 'handoff_fb_rot} == 3', 'fdt rsvmem add 0xF5A00000 0x400000'):
            self.assertIn(text, script)
        self.assertEqual('0x400000', RECORD['framebuffer_reserved_size'])
        v1 = (ROOT / 'boot/diag/erista-d0s-run1.txt').read_text()
        self.assertIn('framebuffer_address=0xF5A00000', v1)
        self.assertEqual('88400', RECORD['loader_size_bytes'])

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
        for phrase in ('not identified', '049102CA', '9802D131', '01FB0000', '0007', 'PASS', '35 C', 'start temperature was not recorded'):
            self.assertIn(phrase, result)
        notes = (ROOT / 'NOTES.md').read_text()
        self.assertIn('D0-S v1\'s hung instruction is still not identified', notes)
        self.assertNotIn('—', result)


if __name__ == '__main__':
    unittest.main()
