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


def switch_beacon_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        if switch_cpu.switch_read_words(memory, symbols['switch_beacon_test_done'], 1) != (0x53574133,):
            raise AssertionError('ARM beacon tests did not finish')
        return switch_cpu.switch_read_words(memory, symbols['switch_beacon_test_results'], 2)


class BeaconCPU(unittest.TestCase):
    def test_station_beacon_bounds(self):
        qemu, gcc, nm = (shutil.which(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        for lto in (False, True):
            with (
                self.subTest(lto=lto),
                tempfile.TemporaryDirectory(prefix='switch-a3-beacon-', dir='/private/tmp') as tmp,
            ):
                path = Path(tmp)
                elf = path / 'beacon.elf'
                subprocess.run(
                    [
                        gcc,
                        '-mcpu=cortex-r4',
                        '-mthumb',
                        '-mfloat-abi=soft',
                        '-mno-unaligned-access',
                        '-Os',
                        '-std=c11',
                        '-Wall',
                        '-Wextra',
                        '-Werror',
                        '-ffreestanding',
                        '-fno-builtin',
                        '-DDEVELOPMENT=1',
                        '-nostdlib',
                        *(['-flto'] if lto else []),
                        '-Wl,-Ttext=0x10000,--entry=switch_beacon_test_start,--build-id=none',
                        str(source / 'beacon.c'),
                        str(source / 'beacon_test.c'),
                        str(source / 'beacon_test_start.S'),
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
                error, checks = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_beacon_results)
                self.assertEqual(error, 0, f'failed ARM assertion at beacon_test.c:{error}')
                self.assertGreaterEqual(checks, 80)


if __name__ == '__main__':
    unittest.main()
