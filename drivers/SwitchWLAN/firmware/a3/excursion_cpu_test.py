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


def switch_excursion_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_excursion_test_results'], 49)
        if switch_cpu.switch_read_words(memory, symbols['switch_excursion_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                control,
                'human-monitor-command',
                {'command-line': 'info registers'},
            )
            raise AssertionError(f'ARM excursion tests did not finish; results={results}; {registers}')
        return results


class ExcursionCPU(unittest.TestCase):
    def test_native_request_is_thumb_symbol(self):
        gcc, objdump = shutil.which('arm-none-eabi-gcc'), shutil.which('arm-none-eabi-objdump')
        if not all((gcc, objdump)):
            self.skipTest('requires arm-none-eabi GCC/binutils')
        with tempfile.TemporaryDirectory(prefix='switch-a3-excursion-link-', dir='/private/tmp') as directory:
            output, caller = Path(directory) / 'symbols.elf', Path(directory) / 'caller.S'
            caller.write_text(
                '.syntax unified\n.thumb\n.global switch_excursion_link\n.thumb_func\n'
                'switch_excursion_link:\n b.w switch_a3_excursion_scan_request\n',
            )
            subprocess.run(
                [
                    gcc,
                    '-mcpu=cortex-r4',
                    '-mthumb',
                    '-nostdlib',
                    '-Wl,-Ttext=0x20e900,--entry=switch_excursion_link,--build-id=none',
                    str(Path(__file__).with_name('excursion_symbols.S')),
                    str(caller),
                    '-o',
                    str(output),
                ],
                check=True,
            )
            disassembly = subprocess.check_output([objdump, '-d', str(output)], text=True)
            self.assertNotIn('blx', disassembly)
            self.assertRegex(disassembly, r'\b2972c <switch_a3_excursion_scan_request>')

    def test_actual_thumb_native_request_and_lifetime(self):
        for lto in (False, True):
            with self.subTest(lto=lto):
                self.switch_check_cpu(lto)

    def switch_check_cpu(self, lto: bool):
        qemu, gcc, nm = (shutil.which(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='switch-a3-excursion-', dir='/private/tmp') as directory:
            path = Path(directory)
            elf = path / 'excursion.elf'
            subprocess.run(
                [
                    gcc,
                    '-mcpu=cortex-r4',
                    '-mthumb',
                    '-mfloat-abi=soft',
                    '-mno-unaligned-access',
                    '-Os',
                    '-std=c11',
                    *(['-flto'] if lto else []),
                    '-Wall',
                    '-Wextra',
                    '-Werror',
                    '-ffreestanding',
                    '-fno-builtin',
                    '-nostdlib',
                    '-DDEVELOPMENT=1',
                    str(source / 'diagnostic.c'),
                    '-Wl,-Ttext=0x10000,--entry=switch_excursion_test_start,--build-id=none',
                    str(source / 'excursion.c'),
                    str(source / 'excursion_test.c'),
                    str(source / 'excursion_test_start.S'),
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
            results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_excursion_results)
            self.assertEqual(results, (0,) * 49, f'failing excursion_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
