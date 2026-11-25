"""A small interpreter for the subset of hush that the D1 script uses, with RAM, the environment and injectable faults.

It models only what the script tests: control flow, `&&`, the environment, RAM reads and writes, `crc32` (big endian
result, as U-Boot stores it), `bootm`, `go` and `read`. It does not model U-Boot, caches or any device; the fault hooks
stand in for the commands that can fail on hardware.
"""

import re
import shlex
import struct
import zlib
from collections.abc import Callable
from typing import ClassVar


class ResetError(Exception):
    """The script reached `reset`."""


class HandoffError(Exception):
    """The script reached `bootm go`, which does not return."""


class Machine:
    def __init__(self, env: dict[str, str] | None = None) -> None:
        self.env = {'devnum': '1', 't210b01': '0', 'sku': '0', 'fdtcontroladdr': '0x80000000', **(env or {})}
        self.mem: dict[int, int] = {}
        self.events: list[tuple] = []
        self.fail: Callable[[tuple], bool] = lambda _event: False
        self.after_loados: Callable[[Machine], None] | None = None
        self.before_read: Callable[[Machine], None] | None = None
        self.exporter_record = True

    def write(self, address: int, data: bytes) -> None:
        for i, byte in enumerate(data):
            self.mem[address + i] = byte

    def read(self, address: int, count: int) -> bytes:
        return bytes(self.mem.get(address + i, 0) for i in range(count))

    def word(self, address: int) -> int:
        return struct.unpack('<I', self.read(address, 4))[0]

    def expand(self, token: str) -> str:
        return re.sub(r'\$\{(\w+)\}', lambda m: self.env.get(m.group(1), ''), token)

    def number(self, token: str) -> int:
        token = self.expand(token)
        return int(token, 0) if token else 0

    def run(self, words: list[str]) -> int:
        name, args = words[0], [self.expand(w) for w in words[1:]]
        handler = getattr(self, 'cmd_' + name.replace('.', '_'), None)
        if handler is None:
            raise KeyError(f'the simulator has no command {name}')
        self.events.append((name, *args))
        return 1 if self.fail((name, *args)) else handler(args)

    def cmd_setenv(self, args: list[str]) -> int:
        self.env[args[0]] = ' '.join(args[1:])
        return 0

    def cmd_test(self, args: list[str]) -> int:
        if args[0] == '-n':
            return int(not args[1])
        terms = [args[i : i + 3] for i in range(0, len(args), 4)]
        return int(not all((a == b) if op == '=' else (a != b) for a, op, b in terms))

    def cmd_itest_l(self, args: list[str]) -> int:
        left, op, right = args
        value = self.word(int(left[1:], 0)) if left.startswith('*') else self.number(left)
        return int(not (value == self.number(right) if op == '==' else False))

    def cmd_echoe(self, _args: list[str]) -> int:
        return 0

    cmd_base = cmd_sleep = cmd_true = cmd_echoe

    def cmd_reset(self, _args: list[str]) -> int:
        raise ResetError

    FRAMEBUFFER: ClassVar[dict[str, str]] = {
        'address': '0xF5A00000',
        'width': '0x2D0',
        'height': '0x500',
        'format': 'a8b8g8r8',
        'rot': '3',
    }

    def cmd_fdt(self, args: list[str]) -> int:
        if args[0] == 'get':
            self.env[args[2]] = '4' if args[1] == 'size' else self.FRAMEBUFFER[args[4]]
        return 0

    def cmd_cp_b(self, args: list[str]) -> int:
        self.write(self.number(args[1]), bytes([0xD0, 0x0D, 0xFE, 0xED]) + (0x300).to_bytes(4, 'little'))
        return 0

    def cmd_mw_b(self, args: list[str]) -> int:
        address, value, count = (self.number(a) for a in args[:3])
        self.write(address, bytes([value]) * count)
        return 0

    def cmd_mw_l(self, args: list[str]) -> int:
        address, value, count = (self.number(a) for a in args[:3])
        self.write(address, struct.pack('<I', value) * count)
        return 0

    def cmd_crc32(self, args: list[str]) -> int:
        source, length, dest = (self.number(a) for a in args)
        self.write(dest, struct.pack('>I', zlib.crc32(self.read(source, length))))
        return 0

    def cmd_bootm(self, args: list[str]) -> int:
        step = args[0]
        if step == 'start':
            return self.bootm_start(self.number(args[1]))
        if step == 'loados':
            image = self.image
            if image is None:
                return 1
            self.write(image['load'], self.read(image['at'] + 64, image['size']))
            if image['at'] and self.after_loados and image['load'] == 0xA7000000:
                self.after_loados(self)
            return 0
        if step == 'prep':
            return 0
        self.events.append(('HANDOFF',))
        raise HandoffError

    image: dict | None = None

    def bootm_start(self, at: int) -> int:
        header = self.read(at, 64)
        magic, _, _, size, load, _, dcrc = struct.unpack('>7I', header[:28])
        checked = at == 0xAA0B0000
        if checked and self.env.get('verify') == 'yes' and zlib.crc32(self.read(at + 64, size)) != dcrc:
            return 1
        if magic != 0x27051956 and checked:
            return 1
        self.image = {
            'at': at if checked else 0,
            'size': size if checked else 0,
            'load': load if checked else 0xA0000000,
        }
        return 0

    def cmd_go(self, args: list[str]) -> int:
        address = self.number(args[0])
        if address in (0xAA001920, 0xAA001A64):
            return self.helper_rc
        if address == 0xA7000000:
            if self.exporter_rc:
                return 1
            if self.exporter_record:
                self.write(0xA001F400, struct.pack('<3I', 0x4D435349, 1, 0x100))
            return 0
        return 1

    exporter_rc = 0
    helper_rc = 1

    def cmd_read(self, args: list[str]) -> int:
        if self.before_read:
            self.before_read(self)
        self.write(self.number(args[2]), bytes(range(256)) * 2)
        return 0


def parse(text: str) -> list:
    """Return the script as nested ('if', cond, then, else) / ('for', words, body) / ('cmd', line) nodes."""
    lines = [raw.strip() for raw in text.splitlines() if raw.strip() and not raw.strip().startswith('#')]

    def block(i: int, stops: tuple[str, ...]) -> tuple[list, int]:
        nodes = []
        while i < len(lines) and lines[i] not in stops:
            line = lines[i]
            if line.startswith('if '):
                cond = line[3:].removesuffix('; then')
                then, i = block(i + 1, ('else', 'fi'))
                other: list = []
                if lines[i] == 'else':
                    other, i = block(i + 1, ('fi',))
                nodes.append(('if', cond, then, other))
            elif line.startswith(('for ', 'while ')):
                body, i = block(i + 1, ('done',))
                nodes.append(('loop', line, body))
            else:
                nodes.append(('cmd', line))
            i += 1
        return nodes, i

    return block(0, ())[0]


def condition(machine: Machine, cond: str) -> bool:
    return all(not machine.run(shlex.split(part.strip())) for part in cond.split(' && '))


def execute(machine: Machine, nodes: list) -> None:
    for node in nodes:
        if node[0] == 'cmd':
            line = node[1]
            if ' && ' in line:
                condition(machine, line)
            else:
                machine.run(shlex.split(line))
        elif node[0] == 'if':
            execute(machine, node[2] if condition(machine, node[1]) else node[3])
        elif node[1].startswith('for '):
            execute(machine, node[2])
        else:
            raise RuntimeError('endless loop reached')


def run_script(machine: Machine, text: str) -> str:
    """Run the script; return how it ended: handoff, reset or the final status."""
    try:
        execute(machine, parse(text))
    except HandoffError:
        return 'handoff'
    except ResetError:
        return 'reset'
    return 'ran off the end'
