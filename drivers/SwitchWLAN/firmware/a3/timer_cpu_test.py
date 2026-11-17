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


def switch_timer_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as timer, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(timer.readline())
        switch_cpu.switch_qmp_command(timer, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(timer, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(timer, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_timer_test_results'], 11)
        if switch_cpu.switch_read_words(memory, symbols['switch_timer_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                timer, 'human-monitor-command', {'command-line': 'info registers'}
            )
            raise AssertionError(f'ARM timer tests did not finish; results={results}; {registers}')
        return results


class TimerCPU(unittest.TestCase):
    def test_clock_and_timer_ownership(self):
        qemu, gcc, nm = (shutil.which(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        for variant in range(5):
            with (
                self.subTest(variant=variant),
                tempfile.TemporaryDirectory(prefix='switch-a3-timer-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = path / 'timer.elf'
                subprocess.run(
                    [
                        gcc,
                        '-mcpu=cortex-r4',
                        '-mthumb',
                        '-mno-unaligned-access',
                        '-Os',
                        '-std=c11',
                        '-Wall',
                        '-Wextra',
                        '-Werror',
                        '-ffreestanding',
                        '-fno-builtin',
                        '-nostdlib',
                        f'-DSWITCH_QUEUE_VARIANT={variant}',
                        '-Wl,-Ttext=0x10000,--entry=switch_timer_test_start,--build-id=none',
                        str(source / 'timer.c'),
                        str(source / 'clock.c'),
                        str(source / 'timer_test.c'),
                        str(source / 'timer_test_start.S'),
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
                results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_timer_results)
                self.assertEqual(results, (0,) * 11, f'failing timer_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
