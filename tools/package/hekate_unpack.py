"""Offline unpacker for the hekate release loader (hekate_ctcaer_*.bin).

The release image is an ARM loader followed by two LZ77 streams. The loader
expands them back to back at IPL_LOAD_ADDR. This tool mirrors that logic on the
host with range checks the original lacks.
"""

import argparse
import hashlib
import struct
import sys

LOADER_BASE = 0x40007000
PAYLOAD_SIZE_LITERALS = (0x55C, 0x560)
PAYLOAD_START_LITERAL = 0x538
PAYLOAD_01_LITERAL = 0x534
MAX_OUTPUT = 0x30000


class LzError(ValueError):
    pass


def read_varsize(data: bytes, pos: int, end: int) -> tuple[int, int]:
    value = 0
    while True:
        if pos >= end:
            raise LzError(f'varint runs past input end at {pos:#x}')
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if value > MAX_OUTPUT:
            raise LzError(f'varint value {value:#x} is implausibly large')
        if not byte & 0x80:
            return value, pos


def lz_uncompress(data: bytes, max_output: int = MAX_OUTPUT) -> bytes:
    end = len(data)
    if end == 0:
        return b''
    marker = data[0]
    pos = 1
    out = bytearray()
    while pos < end:
        symbol = data[pos]
        pos += 1
        if symbol != marker:
            out.append(symbol)
        elif pos >= end:
            raise LzError('marker at end of input')
        elif data[pos] == 0:
            out.append(marker)
            pos += 1
        else:
            length, pos = read_varsize(data, pos, end)
            offset, pos = read_varsize(data, pos, end)
            if offset == 0 or offset > len(out):
                raise LzError(f'offset {offset:#x} outside output {len(out):#x}')
            if len(out) + length > max_output:
                raise LzError(f'output would exceed {max_output:#x}')
            for _ in range(length):
                out.append(out[-offset])
        if len(out) > max_output:
            raise LzError(f'output exceeds {max_output:#x}')
    return bytes(out)


def locate(image: bytes) -> tuple[int, int, int, int]:
    """Return (start_00, size_00, start_01, size_01) file offsets and sizes."""

    def word(offset: int) -> int:
        return struct.unpack_from('<I', image, offset)[0]

    size_00 = word(PAYLOAD_SIZE_LITERALS[0])
    size_01 = word(PAYLOAD_SIZE_LITERALS[1])
    start_00 = word(PAYLOAD_START_LITERAL) - LOADER_BASE
    start_01 = word(PAYLOAD_01_LITERAL) - LOADER_BASE
    if start_00 + size_00 > len(image) or start_01 + size_01 > len(image):
        raise LzError('payload range exceeds image')
    if start_00 + size_00 > start_01:
        raise LzError('payload_00 overlaps payload_01')
    if start_00 % 4 or start_01 % 4:
        raise LzError('payload start is not word aligned')
    return start_00, size_00, start_01, size_01


def unpack(image: bytes) -> tuple[bytes, tuple[int, int, int, int, int]]:
    start_00, size_00, start_01, size_01 = locate(image)
    first = lz_uncompress(image[start_00 : start_00 + size_00])
    second = lz_uncompress(image[start_01 : start_01 + size_01])
    return first + second, (start_00, size_00, start_01, size_01, len(first))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('-o', '--output')
    args = parser.parse_args(argv)
    with open(args.image, 'rb') as handle:
        image = handle.read()
    try:
        out, (s0, n0, s1, n1, first_len) = unpack(image)
    except LzError as err:
        print(f'error: {err}', file=sys.stderr)
        return 1
    print(f'image sha256 {hashlib.sha256(image).hexdigest()} size {len(image)}')
    print(f'payload_00 offset {s0:#x} size {n0:#x}')
    print(f'payload_01 offset {s1:#x} size {n1:#x}')
    print(f'gap between streams {s1 - (s0 + n0)} bytes')
    print(f'trailing bytes {len(image) - (s1 + n1)}')
    print(f'half 0 {first_len:#x} half 1 {len(out) - first_len:#x}')
    print(f'unpacked sha256 {hashlib.sha256(out).hexdigest()} size {len(out)}')
    if args.output:
        with open(args.output, 'wb') as handle:
            handle.write(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
