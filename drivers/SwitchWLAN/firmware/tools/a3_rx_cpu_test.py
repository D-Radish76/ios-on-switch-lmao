import json
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import BinaryIO


def switch_qmp_command(control: BinaryIO, name: str, arguments: dict[str, str] | None = None) -> str:
    message = {'execute': name}
    if arguments is not None:
        message['arguments'] = arguments
    control.write(json.dumps(message).encode() + b'\n')
    while True:
        reply = json.loads(control.readline())
        if 'error' in reply:
            raise RuntimeError(reply)
        if 'return' in reply:
            return str(reply['return'])


def switch_read_words(memory: BinaryIO, address: int, count: int) -> tuple[int, ...]:
    memory.write(f'read {address:#x} {count * 4}\n'.encode())
    reply = memory.readline().decode().split()
    if len(reply) != 2 or reply[0] != 'OK':
        raise RuntimeError(reply)
    return struct.unpack(f'<{count}I', bytes.fromhex(reply[1][2:]))


def switch_rx_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_qmp_command(control, 'qmp_capabilities')
        switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_qmp_command(control, 'stop')
        results = switch_read_words(memory, symbols['switch_rx_test_results'], 31)
        if switch_read_words(memory, symbols['switch_rx_test_done'], 1) != (0x53574133,):
            registers = switch_qmp_command(control, 'human-monitor-command', {'command-line': 'info registers'})
            raise AssertionError(f'ARM test did not finish; results={results}; {registers}')
        return results


def switch_run_arm(
    qemu: str,
    elf: Path,
    path: Path,
    symbols: dict[str, int],
    reader: Callable[[socket.socket, socket.socket, dict[str, int]], tuple[int, ...]] = switch_rx_results,
) -> tuple[int, ...]:
    proc = subprocess.Popen(
        [
            qemu,
            '-M',
            'versatilepb',
            '-cpu',
            'cortex-a15',
            '-m',
            '16M',
            '-display',
            'none',
            '-serial',
            'none',
            '-monitor',
            'none',
            '-audio',
            'none',
            '-device',
            f'loader,file={elf},cpu-num=0',
            '-qtest',
            f'unix:{path}/qtest,server=on,wait=off',
            '-qmp',
            f'unix:{path}/qmp,server=on,wait=off',
            '-S',
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    try:
        deadline = time.monotonic() + 3
        while not (path / 'qtest').exists() or not (path / 'qmp').exists():
            if proc.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('QEMU did not open its control sockets')
            time.sleep(0.01)
        with socket.socket(socket.AF_UNIX) as qmp, socket.socket(socket.AF_UNIX) as qtest:
            qmp.settimeout(3)
            qtest.settimeout(3)
            qmp.connect(str(path / 'qmp'))
            qtest.connect(str(path / 'qtest'))
            return reader(qmp, qtest, symbols)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        proc.stderr.close()


class ReceiveCPU(unittest.TestCase):
    def test_native_symbols_preserve_thumb_calls(self):
        gcc = shutil.which('arm-none-eabi-gcc')
        objdump = shutil.which('arm-none-eabi-objdump')
        if not all((gcc, objdump)):
            self.skipTest('requires arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent.parent / 'a3'
        with tempfile.TemporaryDirectory(prefix='switch-a3-rx-link-', dir='/private/tmp') as directory:
            elf = Path(directory) / 'rx.elf'
            subprocess.run(
                [
                    gcc,
                    '-mcpu=cortex-r4',
                    '-mthumb',
                    '-mno-unaligned-access',
                    '-Os',
                    '-ffreestanding',
                    '-fno-builtin',
                    '-nostdlib',
                    '-Wl,-Ttext=0x20e900',
                    '-Wl,--entry=switch_a3_rx_entry',
                    '-Wl,--build-id=none',
                    str(source / 'rx.c'),
                    str(source / 'clock.c'),
                    str(source / 'rx.S'),
                    str(source / 'rx_symbols.S'),
                    '-o',
                    str(elf),
                ],
                check=True,
                capture_output=True,
            )
            disassembly = subprocess.check_output([objdump, '-d', str(elf)], text=True)
            self.assertNotIn('blx', disassembly)
            self.assertRegex(disassembly, r'beq\.w\s+19cff8 <switch_a3_rx_original>')
            for name in (
                'packet_get',
                'packet_free',
                'malloc_zero',
                'free',
                'packet_fetch',
                'raw_sendup',
                'tsf_read',
                'tsf_receive',
            ):
                self.assertIn(f'<switch_a3_{name}>', disassembly)

    def test_actual_thumb_receiver_and_dma_ownership(self):
        qemu = shutil.which('qemu-system-arm')
        gcc = shutil.which('arm-none-eabi-gcc')
        nm = shutil.which('arm-none-eabi-nm')
        if not all((qemu, gcc, nm)):
            self.skipTest('requires qemu-system-arm and arm-none-eabi GCC/binutils')
        source = Path(__file__).resolve().parent.parent / 'a3'
        with tempfile.TemporaryDirectory(prefix='switch-a3-rx-', dir='/private/tmp') as directory:
            path = Path(directory)
            elf = path / 'rx.elf'
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
                    '-Wl,-Ttext=0x10000',
                    '-Wl,--entry=switch_rx_test_start',
                    '-Wl,--build-id=none',
                    str(source / 'rx.c'),
                    str(source / 'clock.c'),
                    str(source / 'rx.S'),
                    str(source / 'rx_test.c'),
                    str(source / 'rx_test_start.S'),
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
            results = switch_run_arm(qemu, elf, path, symbols)
            self.assertEqual(results, (0,) * 31, f'failing rx_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
