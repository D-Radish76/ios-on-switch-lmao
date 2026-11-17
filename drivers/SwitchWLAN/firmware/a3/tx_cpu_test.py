import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import a3_rx_cpu_test as switch_cpu


def switch_tx_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_tx_test_results'], 82)
        results += switch_cpu.switch_read_words(memory, symbols['switch_timing_test_results'], 22)
        if switch_cpu.switch_read_words(memory, symbols['switch_tx_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                control,
                'human-monitor-command',
                {'command-line': 'info registers'},
            )
            raise AssertionError(f'ARM TX tests did not finish; results={results}; {registers}')
        return results


class TransmitCPU(unittest.TestCase):
    def test_native_pm_tailcall_preserves_thumb_state(self):
        gcc, objdump = shutil.which('arm-none-eabi-gcc'), shutil.which('arm-none-eabi-objdump')
        if not all((gcc, objdump)):
            self.skipTest('requires arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='switch-a3-pm-link-', dir='/private/tmp') as directory:
            elf, observer = Path(directory) / 'pm.elf', Path(directory) / 'observer.S'
            observer.write_text(
                '.syntax unified\n.thumb\n.global switch_a3_channel_pm_status\n'
                '.thumb_func\nswitch_a3_channel_pm_status:\n bx lr\n',
            )
            subprocess.run(
                [
                    gcc,
                    '-mcpu=cortex-r4',
                    '-mthumb',
                    '-nostdlib',
                    '-Wl,-Ttext=0x20e900,--entry=switch_a3_pm_status_entry,--build-id=none',
                    str(source / 'pm_status.S'),
                    str(source / 'pm_symbols.S'),
                    str(observer),
                    '-o',
                    str(elf),
                ],
                check=True,
                capture_output=True,
            )
            disassembly = subprocess.check_output([objdump, '-d', str(elf)], text=True)
            self.assertNotIn('blx', disassembly)
            self.assertRegex(disassembly, r'\b195f30 <switch_a3_pm_status_original>')

    def test_native_symbols_preserve_thumb_calls(self):
        gcc, objdump = shutil.which('arm-none-eabi-gcc'), shutil.which('arm-none-eabi-objdump')
        if not all((gcc, objdump)):
            self.skipTest('requires arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='switch-a3-tx-link-', dir='/private/tmp') as directory:
            elf = Path(directory) / 'tx.elf'
            observer = Path(directory) / 'owned.S'
            names = (
                'switch_a3_scheduler_admit',
                'switch_a3_control_retired',
                'switch_a3_control_fault',
                'switch_a3_control_timer_fault',
                'switch_a3_rx_event',
                'switch_a3_timer_initialize_owned',
                'switch_a3_timer_arm_owned',
                'switch_a3_timer_defer_owned',
                'switch_a3_timer_cancel_owned',
            )
            observer.write_text(
                '.syntax unified\n.thumb\n'
                + ''.join(f'.global {name}\n.thumb_func\n{name}:\n bx lr\n' for name in names)
            )
            subprocess.run(
                [
                    gcc,
                    '-mcpu=cortex-r4',
                    '-mthumb',
                    '-mfloat-abi=soft',
                    '-mno-unaligned-access',
                    '-Os',
                    '-ffreestanding',
                    '-fno-builtin',
                    '-ffunction-sections',
                    '-fdata-sections',
                    '-nostdlib',
                    '-Wl,-Ttext=0x20e900,--entry=switch_a3_tx_entry,--build-id=none',
                    str(source / 'tx.c'),
                    str(source / 'tx.S'),
                    str(source / 'tx_symbols.S'),
                    str(source / 'tx_timing.c'),
                    str(source / 'clock.c'),
                    str(source / 'rx_symbols.S'),
                    str(observer),
                    '-o',
                    str(elf),
                ],
                check=True,
                capture_output=True,
            )
            disassembly = subprocess.check_output([objdump, '-d', str(elf)], text=True)
            self.assertNotIn('blx', disassembly)
            for name, address in (
                ('sendctl', '197754'),
                ('register', '5d6f0'),
                ('unregister', '1b20c8'),
                ('original', '18c7a4'),
                ('status_original', '5d6cc'),
            ):
                self.assertRegex(disassembly, rf'\b{address} <switch_a3_tx_{name}>')

    def test_actual_thumb_ingress_dma_and_completion(self):
        for lto in (False, True):
            with self.subTest(lto=lto):
                self.switch_check_cpu(lto)

    def switch_check_cpu(self, lto: bool):
        qemu, gcc, nm = (shutil.which(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='switch-a3-tx-', dir='/private/tmp') as directory:
            path = Path(directory)
            elf = path / 'tx.elf'
            subprocess.run(
                [
                    gcc,
                    '-mcpu=cortex-r4',
                    '-mthumb',
                    '-mfloat-abi=soft',
                    '-mno-unaligned-access',
                    '-Os',
                    '-std=c11',
                    '-ffunction-sections',
                    '-fdata-sections',
                    *(['-flto'] if lto else []),
                    '-Wall',
                    '-Wextra',
                    '-Werror',
                    '-ffreestanding',
                    '-fno-builtin',
                    '-nostdlib',
                    '-DDEVELOPMENT=1',
                    str(source / 'diagnostic.c'),
                    '-Wl,-Ttext=0x10000,--entry=switch_tx_test_start,--build-id=none,--gc-sections',
                    str(source / 'tx.c'),
                    str(source / 'rx.c'),
                    str(source / 'tx.S'),
                    str(source / 'tx_timing.c'),
                    str(source / 'tx_test.c'),
                    str(source / 'tx_test_start.S'),
                    str(source / 'tx_timing_test.c'),
                    str(source / 'pm_status.S'),
                    '-o',
                    str(elf),
                ],
                check=True,
                capture_output=True,
            )
            symbols = {}
            for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3:
                    symbols[fields[2]] = int(fields[0], 16)
            results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_tx_results)
            self.assertEqual(results, (0,) * 104, f'failing tx_test.c/tx_timing_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
