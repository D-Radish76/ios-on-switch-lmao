"""Offline reader for the Erista BPMP firmware (bpmpfw.bin): its IRAM image and the device accesses it can resolve.

The firmware is never stored in this repository. The scan follows constants through straight runs of code (literal
loads, immediates, add/sub/orr, register moves) and reports loads and stores whose address lands in CAR, the I2C blocks
or PMC. It also resolves calls to the small read-modify-write and store helpers when the address argument is known. It
does not follow values through memory or across calls to other functions, so a missing access is not proof of absence.
"""

import argparse
import hashlib
import re
import struct
import subprocess
from pathlib import Path

HEADER_MAGIC = b'bpmp'
CODE_OFFSET = 0x100
IRAM = 0x40000000
REGIONS = {
    'CAR': (0x60006000, 0x60007000),
    'I2C': (0x7000C000, 0x7000E000),
    'PMC': (0x7000E400, 0x7000E800),
}
SOURCE_OFFSETS = {0x124: 'I2C1', 0x128: 'I2C5', 0x198: 'I2C2', 0x1B8: 'I2C3', 0x3C4: 'I2C4', 0x65C: 'I2C6'}
LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\w+)\s*(.*)$')
LITERAL = re.compile(r'\[pc, #(-?0x[0-9a-f]+|-?\d+)\]')
MEMORY = re.compile(
    r'\[(\w+)(?:, (#-?0x[0-9a-f]+|#-?\d+|-?\w+)(?:, (lsl) #(\d+))?)?\](!?)(?:, (#-?0x[0-9a-f]+|#-?\d+))?'
)
TERMINATORS = ('b', 'bx')
CONDITIONS = ('eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le')


def read_header(data: bytes) -> dict[str, int]:
    if data[:4] != HEADER_MAGIC:
        raise ValueError('not a bpmp image')
    words = struct.unpack_from('<4I', data, 4)
    return {'version': words[1], 'memory_size': words[2], 'entry_offset': words[3]}


def copy_plan(data: bytes) -> list[tuple[int, int, int]]:
    """Return the segments the entry code copies, as (file offset, destination, end), from its pool at 0x210."""
    pool = struct.unpack_from('<16I', data, 0x210)
    return [(pool[1], pool[2], pool[3]), (pool[4], pool[5], pool[6])]


def iram_image(data: bytes) -> bytes:
    """IRAM as the cold path leaves it before the bss clear: both copies applied."""
    image = bytearray(0x40000)
    for source, destination, end in copy_plan(data):
        chunk = data[source : source + (end - destination)]
        image[destination - IRAM : destination - IRAM + len(chunk)] = chunk
    return bytes(image[: copy_plan(data)[1][1] - IRAM + len(data) - copy_plan(data)[1][0]])


def disassemble(image: bytes, workdir: Path) -> str:
    workdir.mkdir(parents=True, exist_ok=True)
    raw = workdir / 'iram.bin'
    raw.write_bytes(image)
    obj, elf = workdir / 'iram0.elf', workdir / 'iram.elf'
    subprocess.run(
        [
            'llvm-objcopy',
            '-I',
            'binary',
            '-O',
            'elf32-littlearm',
            '--rename-section',
            '.data=.text,alloc,load,code',
            raw,
            obj,
        ],
        check=True,
    )
    subprocess.run(['ld.lld', f'-Ttext={IRAM:#x}', '-e', '0', obj, '-o', elf], check=True)
    out = subprocess.run(
        ['llvm-objdump', '-d', '--triple=armv7', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
    )
    return out.stdout


def region(address: int) -> str | None:
    for name, (low, high) in REGIONS.items():
        if low <= address < high:
            return name
    return None


def number(text: str) -> int:
    return int(text.lstrip('#'), 0)


def split_args(args: str) -> list[str]:
    parts, depth, current = [], 0, ''
    for char in args:
        if char in '[{':
            depth += 1
        if char in ']}':
            depth -= 1
        if char == ',' and depth == 0:
            parts.append(current.strip())
            current = ''
        else:
            current += char
    if current.strip():
        parts.append(current.strip())
    return parts


BRANCH_NOT_CALL = ('blt', 'ble', 'blo', 'bls')


def is_call(op: str) -> bool:
    if op in BRANCH_NOT_CALL:
        return False
    return op in ('bl', 'blx') or (op.startswith('bl') and op[2:] in CONDITIONS)


def base_op(op: str) -> str:
    for suffix in CONDITIONS:
        if op.endswith(suffix) and len(op) > len(suffix) and op[: -len(suffix)] in ARITHMETIC | MEMORY_OPS | MOVES:
            return op[: -len(suffix)]
    return op


ARITHMETIC = {'add', 'sub', 'orr', 'bic', 'and', 'eor', 'rsb'}
MOVES = {'mov', 'mvn', 'movw', 'movt', 'ldr'}
MEMORY_OPS = {'ldr', 'str', 'ldrb', 'strb', 'ldrh', 'strh'}


RMW_BODY = ('ldr r3, [r0]', 'bic r3, r3, r1', 'orr r3, r3, r2', 'str r3, [r0]', 'bx lr')
WRITE_BODY = (('str r1, [r0]', 'bx lr'), ('str r0, [r1]', 'bx lr'))


def helpers(dis: str) -> dict[int, str]:
    """Entry addresses of the small functions that read-modify-write or store through a pointer argument."""
    rows = []
    for line in dis.splitlines():
        match = LINE.match(line)
        if match:
            rows.append((int(match.group(1), 16), f'{match.group(2)} {match.group(3).split("@")[0].strip()}'))
    found = {}
    for index, (address, _) in enumerate(rows):
        window = tuple(text for _, text in rows[index : index + len(RMW_BODY)])
        if window == RMW_BODY:
            found[address] = 'rmw'
        for body in WRITE_BODY:
            if tuple(text for _, text in rows[index : index + 2]) == body:
                found[address] = 'store'
    return found


class Scan:
    def __init__(self, image: bytes, calls: dict[int, str] | None = None) -> None:
        self.image = image
        self.calls = calls or {}
        self.registers: dict[str, int] = {}
        self.found: list[dict] = []

    def word(self, address: int) -> int | None:
        offset = address - IRAM
        if 0 <= offset <= len(self.image) - 4:
            return struct.unpack_from('<I', self.image, offset)[0]
        return None

    def value(self, text: str) -> int | None:
        if text.startswith('#'):
            return number(text)
        return self.registers.get(text)

    def record(self, address: int, pc: int, op: str, target: str, args: str) -> None:
        kind = region(address)
        if kind:
            self.found.append(
                {'pc': pc, 'op': op, 'address': address, 'region': kind, 'register': target, 'text': args}
            )

    def step(self, pc: int, op: str, args: str, comment: str) -> None:
        name = base_op(op)
        parts = split_args(args)
        if name == 'ldr' and LITERAL.search(args):
            literal = re.search(r'@ 0x([0-9a-f]+)', comment)
            self.assign(parts[0], self.word(int(literal.group(1), 16)) if literal else None)
            return
        if name in MEMORY_OPS:
            self.memory(pc, op, name, parts, args)
            return
        if not parts:
            return
        destination = parts[0]
        if name in MOVES - {'ldr'}:
            self.move(name, destination, parts)
        elif name in ARITHMETIC and len(parts) >= 3:
            self.assign(destination, self.arithmetic(name, parts))
        elif is_call(op):
            self.call(pc, args)
            for register in ('r0', 'r1', 'r2', 'r3', 'r12'):
                self.registers.pop(register, None)
        elif name not in ('cmp', 'tst', 'teq', 'cmn', 'b', 'bx', 'push', 'stm', 'stmdb', 'nop'):
            self.registers.pop(destination, None)

    def move(self, name: str, destination: str, parts: list[str]) -> None:
        value = self.value(parts[1]) if len(parts) > 1 else None
        if name == 'mvn':
            value = None if value is None else ~value & 0xFFFFFFFF
        elif name == 'movt':
            low = self.registers.get(destination)
            value = None if low is None or len(parts) < 2 else (number(parts[1]) << 16) | (low & 0xFFFF)
        self.assign(destination, value)

    def call(self, pc: int, args: str) -> None:
        target = re.match(r'(?:#)?(?:0x)?([0-9a-f]+)', args)
        kind = self.calls.get(int(target.group(1), 16)) if target else None
        address = self.registers.get('r0') if kind == 'rmw' else self.registers.get('r1' if kind == 'store' else 'r0')
        if kind and address is not None and region(address):
            clear = self.registers.get('r1') if kind == 'rmw' else None
            setbits = self.registers.get('r2') if kind == 'rmw' else self.registers.get('r0')
            self.found.append(
                {'pc': pc, 'op': f'call-{kind}', 'address': address, 'region': region(address), 'register': 'r0',
                 'text': f'clear={clear} set={setbits}', 'clear': clear, 'set': setbits}
            )  # fmt: skip

    def assign(self, destination: str, value: int | None) -> None:
        if value is None:
            self.registers.pop(destination, None)
        else:
            self.registers[destination] = value & 0xFFFFFFFF

    def arithmetic(self, name: str, parts: list[str]) -> int | None:
        left = self.value(parts[1])
        right = self.value(parts[2])
        if len(parts) > 3:
            shift = re.fullmatch(r'lsl #(\d+)', parts[3])
            right = right << int(shift.group(1)) if shift and right is not None else None
        if left is None or right is None:
            return None
        return {
            'add': left + right, 'sub': left - right, 'orr': left | right, 'bic': left & ~right,
            'and': left & right, 'eor': left ^ right, 'rsb': right - left,
        }[name]  # fmt: skip

    def memory(self, pc: int, op: str, name: str, parts: list[str], args: str) -> None:
        match = MEMORY.search(args)
        if not match:
            return
        base, offset, shift, amount, writeback, post = match.groups()
        base_value = self.registers.get(base)
        address = None
        if base_value is not None:
            if post or offset is None:
                address = base_value
            else:
                delta = self.value(offset)
                if delta is not None and shift:
                    delta <<= int(amount)
                address = None if delta is None else base_value + delta
        if address is not None:
            self.record(address & 0xFFFFFFFF, pc, op, parts[0], args)
        if name.startswith('ldr') and parts[0] != 'pc':
            self.registers.pop(parts[0], None)
        if writeback or post:
            step = number(post) if post else (self.value(offset) if offset else None)
            if base_value is not None and step is not None:
                self.registers[base] = (base_value + step) & 0xFFFFFFFF

    def run(self, dis: str) -> list[dict]:
        for line in dis.splitlines():
            match = LINE.match(line)
            if not match:
                continue
            address, op, rest = match.groups()
            args, _, comment = rest.partition('@')
            args = args.split('//')[0].strip()
            pc = int(address, 16)
            self.step(pc, op, args, '@' + comment)
            ends = (
                (op == 'b' and not args.startswith('#')) or op in ('bx', 'b') or (op.startswith('pop') and 'pc' in args)
            )
            if ends or (op.startswith('mov') and args.startswith('pc,')):
                self.registers.clear()
        return self.found


def fmt(value: int | None) -> str:
    return '?' if value is None else f'{value:08x}'


def report(found: list[dict]) -> str:
    lines = []
    for item in found:
        note = SOURCE_OFFSETS.get(item['address'] - 0x60006000, '')
        if 'clear' in item:
            note = f'clear {fmt(item["clear"])} set {fmt(item["set"])} {note}'.strip()
        lines.append(
            f'{item["pc"]:08x} {item["op"]:<10} {item["region"]} {item["address"]:08x} {item["register"]:<4} {note}'
        )
    return '\n'.join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    parser.add_argument('--sha256', help='refuse a file whose SHA-256 differs')
    parser.add_argument('--workdir', type=Path, default=Path('/tmp'))
    args = parser.parse_args()
    data = args.firmware.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if args.sha256 and digest != args.sha256:
        raise SystemExit(f'SHA-256 {digest} does not match {args.sha256}')
    header = read_header(data)
    print(f'sha256 {digest} size {len(data)} header {header}')
    image = iram_image(data)
    dis = disassemble(image, args.workdir)
    print(report(Scan(image, helpers(dis)).run(dis)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
