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


def switch_channel_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as channel, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(channel.readline())
        switch_cpu.switch_qmp_command(channel, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(channel, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(channel, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_channel_test_results'], 26)
        if switch_cpu.switch_read_words(memory, symbols['switch_channel_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                channel, 'human-monitor-command', {'command-line': 'info registers'}
            )
            raise AssertionError(f'ARM channel tests did not finish; results={results}; {registers}')
        return results


class ChannelCPU(unittest.TestCase):
    def test_native_gate_preserves_thumb_tailcalls(self):
        gcc, objdump = shutil.which('arm-none-eabi-gcc'), shutil.which('arm-none-eabi-objdump')
        if not all((gcc, objdump)):
            self.skipTest('requires arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='switch-a3-channel-link-', dir='/private/tmp') as directory:
            elf, observer = Path(directory) / 'channel.elf', Path(directory) / 'observer.S'
            observer.write_text(
                '.syntax unified\n.thumb\n.global switch_a3_channel_may_leave\n'
                '.thumb_func\nswitch_a3_channel_may_leave:\n movs r0, #1\n bx lr\n',
            )
            subprocess.run(
                [
                    gcc,
                    '-mcpu=cortex-r4',
                    '-mthumb',
                    '-nostdlib',
                    '-Wl,-Ttext=0x20e900,--entry=switch_a3_channel_gate,--build-id=none',
                    str(source / 'channel_gate.S'),
                    str(source / 'channel_symbols.S'),
                    str(observer),
                    '-o',
                    str(elf),
                ],
                check=True,
                capture_output=True,
            )
            disassembly = subprocess.check_output([objdump, '-d', str(elf)], text=True)
            self.assertNotIn('blx', disassembly)
            self.assertRegex(disassembly, r'\b193774 <switch_a3_channel_original>')
            self.assertRegex(disassembly, r'\b2586c <switch_a3_channel_mac_enable>')
            self.assertRegex(disassembly, r'\b1cfb2e <switch_a3_channel_cleanup>')

    def test_owned_retune_and_pm_gate(self):
        qemu, gcc, nm = (shutil.which(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent
        for lto in (False, True):
            with (
                self.subTest(lto=lto),
                tempfile.TemporaryDirectory(prefix='switch-a3-channel-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = path / 'channel.elf'
                subprocess.run(
                    [
                        gcc,
                        *(['-flto'] if lto else []),
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
                        '-DDEVELOPMENT=1',
                        '-Wl,-Ttext=0x10000,--entry=switch_channel_test_start,--build-id=none',
                        str(source / 'diagnostic.c'),
                        str(source / 'channel.c'),
                        str(source / 'channel_gate.S'),
                        str(source / 'channel_test.c'),
                        str(source / 'channel_test_start.S'),
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
                results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_channel_results)
                self.assertEqual(results, (0,) * 26, f'failing channel_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
