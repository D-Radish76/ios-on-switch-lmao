"""Finds clock controller (CAR) stores and clock_enable callers in an unpacked hekate image.

Input is the raw unpacked image (hekate_unpack.py), its load address and the
llvm-objdump text of it. The scan is linear and per function: it tracks
constants loaded by movs, ldr from the literal pool and lsls, and reports a
store only when both the address and the value are known. Stores through
computed or table-driven addresses are not reported; the callers of the
table-driven helper are listed instead.
"""

import argparse
import bisect
import re
import struct
import sys

CAR_BASE = 0x60006000
CAR_SIZE = 0x1000
LINE = re.compile(r'^\s*([0-9a-f]{8}):\s+(.*)$')
LDR_PC = re.compile(r'ldr\s+(r\d+), \[pc, #0x[0-9a-f]+\]\s+@ 0x([0-9a-f]+)')
MOV = re.compile(r'movs?\s+(r\d+), #(-?0x[0-9a-f]+|\d+)')
LSL = re.compile(r'lsls\s+(r\d+), (r\d+), #(0x[0-9a-f]+|\d+)')
STR = re.compile(r'str\s+(r\d+), \[(r\d+)(?:, #(0x[0-9a-f]+))?\]')
BL = re.compile(r'bl\s+0x([0-9a-f]+)')
WRITES_DEST = re.compile(r'(?!cmp|tst|push|str)\w+\s+(r\d+)')

REG_NAMES = {
    0x004: 'RST_DEVICES_L',
    0x008: 'RST_DEVICES_H',
    0x010: 'CLK_OUT_ENB_L',
    0x014: 'CLK_OUT_ENB_H',
    0x0A8: 'PLLP_OUTB',
    0x124: 'CLK_SOURCE_I2C1',
    0x128: 'CLK_SOURCE_I2C5',
    0x198: 'CLK_SOURCE_I2C2',
    0x1B8: 'CLK_SOURCE_I2C3',
    0x3C4: 'CLK_SOURCE_I2C4',
    0x65C: 'CLK_SOURCE_I2C6',
    0x300: 'RST_DEV_L_SET',
    0x304: 'RST_DEV_L_CLR',
    0x308: 'RST_DEV_H_SET',
    0x30C: 'RST_DEV_H_CLR',
    0x320: 'CLK_ENB_L_SET',
    0x324: 'CLK_ENB_L_CLR',
    0x328: 'CLK_ENB_H_SET',
    0x32C: 'CLK_ENB_H_CLR',
}
I2C_BITS = {0x300: 12, 0x304: 12, 0x320: 12, 0x324: 12, 0x308: 15, 0x30C: 15, 0x328: 15, 0x32C: 15}


def parse(text: str) -> tuple[list[tuple[int, str]], dict[int, list[int]]]:
    insns = []
    calls: dict[int, list[int]] = {}
    for line in text.splitlines():
        match = LINE.match(line)
        if not match:
            continue
        addr, body = int(match.group(1), 16), match.group(2)
        insns.append((addr, body))
        target = BL.match(body)
        if target:
            calls.setdefault(int(target.group(1), 16), []).append(addr)
    return insns, calls


def function_starts(insns: list[tuple[int, str]]) -> list[int]:
    return sorted(a for a, body in insns if body.startswith('push') and 'lr' in body)


def owner(starts: list[int], addr: int) -> int:
    index = bisect.bisect_right(starts, addr) - 1
    return starts[index] if index >= 0 else 0


def car_stores(image: bytes, base: int, insns: list[tuple[int, str]]) -> list[tuple[int, int, int, int | None]]:
    starts = function_starts(insns)
    regs: dict[str, int] = {}
    current = None
    found = []
    for addr, body in insns:
        func = owner(starts, addr)
        if func != current:
            current, regs = func, {}
        done = _step(image, base, body, regs)
        if done is not None:
            offset, value = done
            found.append((func, addr, offset, value))
    return found


def _step(image: bytes, base: int, body: str, regs: dict[str, int]) -> tuple[int, int | None] | None:
    if (m := LDR_PC.match(body)) is not None:
        pool = int(m.group(2), 16) - base
        regs[m.group(1)] = struct.unpack_from('<I', image, pool)[0] if 0 <= pool <= len(image) - 4 else 0
    elif (m := MOV.match(body)) is not None:
        regs[m.group(1)] = int(m.group(2), 0) & 0xFFFFFFFF
    elif (m := LSL.match(body)) is not None and m.group(2) in regs:
        regs[m.group(1)] = (regs[m.group(2)] << int(m.group(3), 0)) & 0xFFFFFFFF
    elif (m := STR.match(body)) is not None:
        address = regs.get(m.group(2))
        if address is not None and CAR_BASE <= address < CAR_BASE + CAR_SIZE:
            return address - CAR_BASE + int(m.group(3) or '0', 0), regs.get(m.group(1))
    elif body.startswith('bl'):
        for name in ('r0', 'r1', 'r2', 'r3'):
            regs.pop(name, None)
    elif (m := WRITES_DEST.match(body)) is not None:
        regs.pop(m.group(1), None)
    return None


def touches_i2c(offset: int, value: int | None) -> str:
    bit = I2C_BITS.get(offset)
    if bit is None:
        return ''
    if value is None:
        return 'value unknown'
    return 'TOUCHES I2C BIT' if value >> bit & 1 else ''


def callers_of(calls: dict[int, list[int]], starts: list[int], target: int) -> list[int]:
    return sorted({owner(starts, a) for a in calls.get(target, [])})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('disassembly')
    parser.add_argument('--base', type=lambda s: int(s, 0), default=0x40008000)
    parser.add_argument(
        '--target', type=lambda s: int(s, 0), action='append', default=[], help='function whose callers to list'
    )
    args = parser.parse_args(argv)
    with open(args.image, 'rb') as handle:
        image = handle.read()
    with open(args.disassembly) as handle:
        insns, calls = parse(handle.read())
    starts = function_starts(insns)
    for func, addr, offset, value in car_stores(image, args.base, insns):
        name = REG_NAMES.get(offset, f'{offset:#05x}')
        shown = 'unknown' if value is None else f'{value:#010x}'
        print(f'{func:#x} {addr:#x} {name} {shown} {touches_i2c(offset, value)}'.rstrip())
    for target in args.target:
        print(f'callers of {target:#x}: ' + ' '.join(f'{c:#x}' for c in callers_of(calls, starts, target)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
