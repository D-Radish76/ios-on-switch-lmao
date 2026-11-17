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


def switch_limit_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        if switch_cpu.switch_read_words(memory, symbols['switch_rx_limit_test_done'], 1) != (0x53574133,):
            raise AssertionError('ARM receive admission tests did not finish')
        return switch_cpu.switch_read_words(memory, symbols['switch_rx_limit_test_results'], 3)


class ReceiveLimit(unittest.TestCase):
    def test_candidate_reserves_native_fetch_opportunity(self):
        qemu, gcc, nm = (shutil.which(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        for candidate, lto in ((0, False), (1, False), (0, True), (1, True)):
            with (
                self.subTest(candidate=candidate, lto=lto),
                tempfile.TemporaryDirectory(prefix='switch-a3-rx-limit-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = path / 'limit.elf'
                subprocess.run(
                    [
                        gcc,
                        *(['-flto'] if lto else []),
                        '-mcpu=cortex-r4',
                        '-mthumb',
                        '-mno-unaligned-access',
                        '-Os',
                        '-Wall',
                        '-Wextra',
                        '-Werror',
                        '-ffreestanding',
                        '-fno-builtin',
                        '-nostdlib',
                        f'-DSWITCH_A3_CANDIDATE={candidate}',
                        '-DDEVELOPMENT=1',
                    str(source / 'diagnostic.c'),
                    '-Wl,-Ttext=0x10000,--entry=switch_rx_limit_test_start,--build-id=none',
                        str(source / 'rx.c'),
                        str(source / 'rx_limit_test.c'),
                        str(source / 'rx_limit_test_start.S'),
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
                results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_limit_results)
                self.assertEqual(results, (0, 3 if candidate else 4, 4))


if __name__ == '__main__':
    unittest.main()
