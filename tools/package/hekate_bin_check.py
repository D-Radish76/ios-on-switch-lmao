"""Checks a hekate release binary against what the source says (ERISTA_HEKATE_BINARY.md).

It prints each file's SHA-256, then looks for the six-row I2C clock table of `bdk/soc/clock.c` and for 32-bit literals
the source names. A hit means the bytes are present, not that the code that uses them runs.
"""

import argparse
import hashlib
import struct
from pathlib import Path

SOURCE = 6
I2C_TABLE = (
    ('I2C1', 0x300, 0x320, 0x124, 12, 3),
    ('I2C2', 0x308, 0x328, 0x198, 22, 0),
    ('I2C3', 0x310, 0x330, 0x1B8, 3, 0),
    ('I2C4', 0x430, 0x440, 0x3C4, 7, 3),
    ('I2C5', 0x308, 0x328, 0x128, 15, 0),
    ('I2C6', 0x290, 0x284, 0x65C, 6, 3),
)
LITERALS = {
    'hekate': {
        'PLLP_OUTB value written by hw_init': 0x00030003,
        'PLLP_OUTB register address': 0x600060A8,
        'CLK_SOURCE_I2C1 address': 0x60006124,
        'CLK_SOURCE_I2C5 address': 0x60006128,
        'CLK_SOURCE_PWM address': 0x60006110,
        'IPL_LOAD_ADDR': 0x40008000,
        'NYX_STORAGE_ADDR': 0xED000000,
        'EXT_PAYLOAD_ADDR': 0xC0000000,
        'PATCHED_RELOC_ENTRY': 0x40010000,
        'PATCHED_RELOC_STACK': 0x40007000,
    },
    'nyx': {
        'PLLP_OUTB value written by hw_init': 0x00030003,
        'PLLP_OUTB register address': 0x600060A8,
        'CLK_SOURCE_I2C1 address': 0x60006124,
        'CLK_SOURCE_I2C5 address': 0x60006128,
        'CLK_SOURCE_PWM address': 0x60006110,
        'NYX_STORAGE_ADDR': 0xED000000,
        'EXT_PAYLOAD_ADDR': 0xC0000000,
        'PATCHED_RELOC_ENTRY': 0x40010000,
        'PATCHED_RELOC_STACK': 0x40007000,
    },
}


STRINGS = {
    'hekate': (b'hekate v6.5.5', struct.pack('<I', 0x43544349)),
    'nyx': (struct.pack('<I', 0x43544349),),
}


def row(reset: int, enable: int, source: int, index: int, divider: int) -> bytes:
    return struct.pack('<HHHBB', reset, enable, source, index | (SOURCE << 5), divider)


def table() -> bytes:
    return b''.join(row(reset, enable, source, index, div) for _, reset, enable, source, index, div in I2C_TABLE)


def find(data: bytes, needle: bytes, step: int = 1) -> list[int]:
    found, start = [], 0
    while (at := data.find(needle, start)) >= 0:
        if at % step == 0:
            found.append(at)
        start = at + 1
    return found


def arm_mov_30000(data: bytes) -> list[int]:
    """Offsets of ARM `mov rd, #0x30000` (and `movs`), the first half of 0x30003 built with an `orr rd, rd, #3`."""
    words = struct.unpack_from(f'<{len(data) // 4}I', data)
    return [i * 4 for i, w in enumerate(words) if w & 0xFFEF0FFF == 0xE3A00803]


def check(name: str, data: bytes) -> list[str]:
    lines = [f'{name} sha256 {hashlib.sha256(data).hexdigest()} size {len(data)}']
    whole = find(data, table())
    lines.append(
        f'I2C clock table (6 rows, 48 bytes): {"at " + ", ".join(f"{a:#x}" for a in whole) if whole else "NOT FOUND"}'
    )
    if not whole:
        for label, *fields in I2C_TABLE:
            full = find(data, row(*fields))
            prefix = find(data, row(*fields)[:7])
            shown = ', '.join(f'{a:#x}' for a in full) or 'no'
            tails = ', '.join(f'{a:#x} (next byte {data[a + 7 : a + 8].hex() or "-"})' for a in prefix) or 'no'
            lines.append(f'  row {label}: 8 byte match {shown}; first 7 bytes {tails}')
    for label, value in LITERALS[name].items():
        hits = find(data, struct.pack('<I', value), 4)
        lines.append(
            f'literal {value:#010x} {label}: {len(hits)} at {", ".join(f"{a:#x}" for a in hits[:6]) or "none"}'
        )
    for text in STRINGS[name]:
        at = data.find(text)
        lines.append(f'identity {text!r}: {f"at {at:#x}" if at >= 0 else "not found"}')
    moves = ', '.join(f'{a:#x}' for a in arm_mov_30000(data)[:6]) or 'none'
    lines.append(f'ARM mov rd, #0x30000 (0x30003 in two instructions): {moves}')
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hekate', type=Path, help='hekate_ctcaer_6.5.5.bin')
    parser.add_argument('--nyx', type=Path, help='nyx.bin')
    args = parser.parse_args()
    for name, path in (('hekate', args.hekate), ('nyx', args.nyx)):
        if path:
            print('\n'.join(check(name, path.read_bytes())))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
