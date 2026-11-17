import functools
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


def switch_control_results(
    qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int], count: int = 14
) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_control_test_results'], count)
        if switch_cpu.switch_read_words(memory, symbols['switch_control_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                control, 'human-monitor-command', {'command-line': 'info registers'}
            )
            raise AssertionError(f'ARM control tests did not finish; results={results}; {registers}')
        return results


class ControlCPU(unittest.TestCase):
    switch_sources = ('control.c', 'control_test.c', 'control_test_start.S')
    switch_cases = 14
    switch_link_flags = ()

    def test_control_protocol_in_development_and_release(self):
        qemu, gcc, nm = (shutil.which(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        for development in (0, 1):
            with (
                self.subTest(development=development),
                tempfile.TemporaryDirectory(prefix='switch-a3-control-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = path / 'control.elf'
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
                        f'-DDEVELOPMENT={development}',
                        '-Wl,-Ttext=0x10000,--entry=switch_control_test_start,--build-id=none',
                        *self.switch_link_flags,
                        *(str(source / name) for name in self.switch_sources),
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
                results = switch_cpu.switch_run_arm(
                    qemu, elf, path, symbols, functools.partial(switch_control_results, count=self.switch_cases)
                )
                self.assertEqual(results, (0,) * self.switch_cases, f'failing control test line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
