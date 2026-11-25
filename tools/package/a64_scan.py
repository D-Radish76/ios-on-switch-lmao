"""Offline reader for the pinned AArch64 firmware (BL31 and U-Boot): device accesses and call arguments it can resolve.

It follows constants through straight runs of code (mov and movk immediates, adrp and add, literal loads, add, sub,
orr, register moves), reports loads and stores whose address lands in CAR, the I2C blocks or PMC, and reports calls
that pass such an address or an I2C periph number in x0 to x3. A callee that only returns a device address gives it
to its caller. Values are not followed through memory or through other calls, so a missing access is not proof that
none exists.
"""

import argparse
import re
import subprocess
from pathlib import Path

from bpmpfw_scan import region, split_args

LINE = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\w[\w.]*)\s*(.*)$')
CAR = 0x60006000
SOURCE_OFFSETS = {0x124: 'I2C1', 0x128: 'I2C5', 0x198: 'I2C2', 0x1B8: 'I2C3', 0x3C4: 'I2C4', 0x65C: 'I2C6'}
PERIPH_IDS = {12: 'I2C1', 54: 'I2C2', 67: 'I2C3', 103: 'I2C4', 47: 'I2C5', 166: 'I2C6'}
ARGUMENTS = ('x0', 'x1', 'x2', 'x3')
CALLER_SAVED = tuple(f'x{n}' for n in range(19))
ENDS = ('ret', 'br', 'b', 'eret')
MEMORY = re.compile(
    r'\[(\w+)(?:, (#-?0x[0-9a-f]+|#-?\d+|\w+)(?:, (?:lsl|sxtw|uxtw) #?(\d+))?)?\](!?)(?:, (#-?0x[0-9a-f]+|#-?\d+))?'
)
MEMORY_OPS = ('ldr', 'str', 'ldrb', 'strb', 'ldrh', 'strh', 'ldur', 'stur', 'ldp', 'stp', 'ldrsw', 'ldar', 'stlr')
MASK = (1 << 64) - 1


def name(register: str) -> str:
    if register in ('xzr', 'wzr'):
        return 'zero'
    return 'x' + register[1:] if register[0] in 'wx' and register[1:].isdigit() else register


def number(text: str) -> int:
    return int(text.lstrip('#'), 0)


def disassemble(binary: Path, base: int, workdir: Path) -> str:
    workdir.mkdir(parents=True, exist_ok=True)
    obj, elf = workdir / 'a64.o', workdir / 'a64.elf'
    subprocess.run(
        ['llvm-objcopy', '-I', 'binary', '-O', 'elf64-littleaarch64', '--rename-section', '.data=.text,alloc,load,code',
         binary, obj],
        check=True,
    )  # fmt: skip
    subprocess.run(['ld.lld', f'-Ttext={base:#x}', '-e', '0', obj, '-o', elf], check=True)
    return subprocess.run(
        ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
    ).stdout


def rows(dis: str) -> list[tuple[int, str, str]]:
    found = []
    for line in dis.splitlines():
        match = LINE.match(line) or re.match(r'^\s*([0-9a-f]+):\s+(\w[\w.]*)\s*(.*)$', line)
        if match:
            found.append((int(match.group(1), 16), match.group(2), match.group(3).split('//')[0].strip()))
    return found


def constant_returns(listing: list[tuple[int, str, str]]) -> dict[int, int]:
    """Return the functions of a few instructions that leave a device address in x0 and return, by entry address."""
    found = {}
    for index in range(len(listing)):
        scan = Scan(b'', 0, {})
        for _, op, args in listing[index : index + 5]:
            scan.step(0, op, args)
            if op == 'ret':
                value = scan.registers.get('x0')
                if value is not None and region(value):
                    found[listing[index][0]] = value
                break
            if op in ENDS:
                break
    return found


class Scan:
    def __init__(self, image: bytes, base: int, returns: dict[int, int]) -> None:
        self.image, self.base, self.returns = image, base, returns
        self.registers: dict[str, int] = {}
        self.found: list[dict] = []

    def value(self, text: str) -> int | None:
        if text.startswith('#'):
            return number(text)
        return 0 if name(text) == 'zero' else self.registers.get(name(text))

    def assign(self, register: str, value: int | None) -> None:
        if name(register) == 'zero':
            return
        if value is None:
            self.registers.pop(name(register), None)
        else:
            self.registers[name(register)] = value & MASK

    def word(self, address: int, size: int = 4) -> int | None:
        offset = address - self.base
        if 0 <= offset <= len(self.image) - size:
            return int.from_bytes(self.image[offset : offset + size], 'little')
        return None

    def step(self, pc: int, op: str, args: str) -> None:
        parts = split_args(args)
        if op in MEMORY_OPS:
            self.memory(pc, op, parts, args)
        elif op in ('mov', 'movz', 'movn', 'movk') and len(parts) >= 2:
            self.move(op, parts)
        elif op == 'adrp' and len(parts) == 2:
            self.assign(parts[0], int(parts[1].split()[0], 0))
        elif op in ('add', 'sub', 'orr', 'and', 'eor') and len(parts) >= 3:
            self.assign(parts[0], self.arithmetic(op, parts))
        elif op == 'bl':
            self.call(pc, args)
        elif (
            op not in ('cmp', 'tst', 'b', 'cbz', 'cbnz', 'tbz', 'tbnz', 'nop', 'ret', 'br')
            and not op.startswith('b.')
            and parts
        ):
            self.registers.pop(name(parts[0]), None)

    def move(self, op: str, parts: list[str]) -> None:
        if op == 'movk':
            low = self.registers.get(name(parts[0]))
            shift = number(parts[2].split('#')[1]) if len(parts) > 2 else 0
            self.assign(parts[0], None if low is None else (low & ~(0xFFFF << shift)) | (number(parts[1]) << shift))
            return
        value = self.value(parts[1])
        if value is not None and len(parts) > 2:
            value <<= number(parts[2].split('#')[1])
        if op == 'movn' and value is not None:
            value = ~value
        self.assign(parts[0], value)

    def arithmetic(self, op: str, parts: list[str]) -> int | None:
        left, right = self.value(parts[1]), self.value(parts[2])
        if len(parts) > 3:
            shift = re.fullmatch(r'lsl #(\d+)', parts[3])
            right = right << int(shift.group(1)) if shift and right is not None else None
        if left is None or right is None:
            return None
        return {
            'add': left + right,
            'sub': left - right,
            'orr': left | right,
            'and': left & right,
            'eor': left ^ right,
        }[op]

    def memory(self, pc: int, op: str, parts: list[str], args: str) -> None:
        if len(parts) == 2 and not parts[1].startswith('['):
            literal = re.match(r'(?:#)?(0x[0-9a-f]+|-?\d+)', parts[1])
            self.assign(
                parts[0], self.word(int(literal.group(1), 0), 8 if parts[0][0] == 'x' else 4) if literal else None
            )
            return
        match = MEMORY.search(args)
        if not match:
            return
        base, offset, shift, writeback, post = match.groups()
        known = self.registers.get(name(base))
        address = None
        if known is not None:
            delta = 0
            if offset and not post:
                delta = self.value(offset) if offset.startswith('#') else self.registers.get(name(offset))
                delta = None if delta is None else delta << int(shift or 0)
            address = None if delta is None else (known + delta) & MASK
        if address is not None and region(address):
            self.found.append({'op': op, 'pc': pc, 'address': address, 'region': region(address), 'register': parts[0]})
        if op.startswith(('ldr', 'ldur')) or op == 'ldp':
            for register in parts[: 2 if op == 'ldp' else 1]:
                self.registers.pop(name(register), None)
        if (writeback or post) and known is not None:
            step = number(post) if post else self.value(offset or '#0')
            if step is not None:
                self.registers[name(base)] = (known + step) & MASK

    def call(self, pc: int, args: str) -> None:
        target = int(args.split(maxsplit=1)[0], 0)
        seen = {r: self.registers[r] for r in ARGUMENTS if r in self.registers}
        interesting = {r: v for r, v in seen.items() if region(v) or v in PERIPH_IDS}
        if interesting:
            self.found.append({'op': 'call', 'pc': pc, 'address': target, 'region': 'call', 'arguments': interesting})
        for register in CALLER_SAVED:
            self.registers.pop(register, None)
        if target in self.returns:
            self.registers['x0'] = self.returns[target]

    def run(self, listing: list[tuple[int, str, str]]) -> list[dict]:
        for pc, op, args in listing:
            self.step(pc, op, args)
            if (op in ENDS and not args.startswith('#')) or op == 'ret':
                self.registers.clear()
        return self.found


def report(found: list[dict]) -> str:
    lines = []
    for item in found:
        if item['op'] == 'call':
            shown = ' '.join(f'{r}={v:#x}' for r, v in item['arguments'].items())
            lines.append(f'{item["pc"]:08x} call {item["address"]:08x} {shown}')
        else:
            note = SOURCE_OFFSETS.get(item['address'] - CAR, '') if item['region'] == 'CAR' else ''
            head = f'{item["pc"]:08x} {item["op"]:<6} {item["region"]} {item["address"]:08x}'
            lines.append(f'{head} {item["register"]:<4} {note}'.rstrip())
    return '\n'.join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--base', type=lambda x: int(x, 0), required=True)
    parser.add_argument('--workdir', type=Path, default=Path('/tmp'))
    args = parser.parse_args()
    dis = disassemble(args.binary, args.base, args.workdir)
    listing = rows(dis)
    scan = Scan(args.binary.read_bytes(), args.base, constant_returns(listing))
    print(report(scan.run(listing)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
