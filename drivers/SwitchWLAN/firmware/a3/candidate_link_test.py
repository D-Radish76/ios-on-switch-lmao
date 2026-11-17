import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import payload_header

SWITCH_SOURCES = (
    'bootstrap.S',
    'bootstrap.c',
    'beacon.c',
    'candidate_control.c',
    'clock.c',
    'diagnostic.c',
    'rom_candidate.c',
    'rx.c',
    'rx.S',
    'rx_symbols.S',
    'ack.c',
    'ack_symbols.S',
    'timer.c',
    'timer_symbols.S',
    'channel.c',
    'channel_gate.S',
    'channel_symbols.S',
    'pm_status.S',
    'pm.S',
    'pm_symbols.S',
    'tx.c',
    'tx_timing.c',
    'tx.S',
    'tx_symbols.S',
    'excursion.c',
    'excursion_symbols.S',
    'scheduler.c',
)
SWITCH_ROOTS = (
    'channel_gate',
    'pm_status_entry',
    'pm_prepare_entry',
    'pm_submit_entry',
    'pm_set_entry',
    'tx_entry',
    'tx_radio_status',
)


def switch_candidate_command(gcc: str, source: Path, elf: Path, development: int) -> list[str]:
    return [
        gcc,
        '-flto',
        '-DSWITCH_A3_CANDIDATE=1',
        '-mcpu=cortex-r4',
        '-mthumb',
        '-mfloat-abi=soft',
        '-mno-unaligned-access',
        '-Os',
        '-fno-caller-saves',
        '-fno-partial-inlining',
        '-fno-schedule-insns2',
        '-fno-tree-loop-optimize',
        '-fno-optimize-sibling-calls',
        '-fno-ipa-bit-cp',
        '-ffreestanding',
        '-fno-builtin',
        '-fno-stack-protector',
        '-fno-unwind-tables',
        '-fno-asynchronous-unwind-tables',
        '-ffunction-sections',
        '-fdata-sections',
        '-nostdlib',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-Wl,--build-id=none,--gc-sections,--fatal-warnings,--no-warn-rwx-segments',
        f'-DDEVELOPMENT={development}',
        '-T',
        str(source / 'candidate.ld'),
        *('-Wl,--undefined=switch_a3_' + name for name in SWITCH_ROOTS),
        *(str(source / name) for name in SWITCH_SOURCES),
        '-o',
        str(elf),
    ]


class CandidateLink(unittest.TestCase):
    def test_candidate_preserves_station_heap_and_thumb_hooks(self):
        gcc, objcopy = shutil.which('arm-none-eabi-gcc'), shutil.which('arm-none-eabi-objcopy')
        if not all((gcc, objcopy)):
            self.skipTest('requires arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='switch-a3-candidate-', dir='/private/tmp') as directory:
            for development in (0, 1):
                with self.subTest(development=development):
                    elf, binary = Path(directory) / 'candidate.elf', Path(directory) / 'candidate.bin'
                    subprocess.run(
                        switch_candidate_command(gcc, source, elf, development),
                        check=True,
                        capture_output=True,
                    )
                    subprocess.run([objcopy, '-O', 'binary', str(elf), str(binary)], check=True)
                    symbols = payload_header.switch_a3_symbols(elf.read_bytes())
                    begin, end = symbols['switch_a3_island_start'], symbols['switch_a3_island_end']
                    self.assertEqual(begin, 0x20E7DC)
                    self.assertLessEqual(end - begin, 16384)
                    self.assertGreaterEqual(0x23DF6C - end, 178064)
                    self.assertEqual(symbols['switch_a3_tx_phy_chanspec'], 0x8A13D)
                    for name in SWITCH_ROOTS:
                        address = symbols['switch_a3_' + name]
                        self.assertTrue(address & 1, name)
                        self.assertTrue(begin <= (address & ~1) < end, name)
                    if not development:
                        self.assertNotIn(b'switch_awdl_deadline', binary.read_bytes())
                        self.assertNotIn('switch_a3_control_deadline_snapshot', symbols)
                        self.assertFalse([
                            name for name in symbols
                            if name.startswith(('switch_a3_diagnostic', 'switch_a3_control_record'))
                            or 'ga3diag' in name.lower()
                        ])
                    else:
                        self.assertIn('switch_a3_diagnostic', symbols)
                    self.assertIn(
                        'switch_wlan_a3_candidate_payload',
                        payload_header.switch_a3_header(
                            elf.read_bytes(),
                            binary.read_bytes(),
                            candidate=True,
                        ),
                    )


if __name__ == '__main__':
    unittest.main()
