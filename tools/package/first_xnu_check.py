"""Checks for the first-XNU observation package, read from the built artifacts.

kernel KERNEL    exits 0 only if the kernel's start.s was built with SWITCH_EARLY_TRACE (the early marks draw).
margin LOADER    prints the loader image's room below U-Boot's stamps slot and the SD reference page.
"""

import argparse
import struct
import sys
from pathlib import Path

LOAD = 0xA0000000
STAMPS_SLOT = 0xA001EF00
SD_REFERENCE = 0xA001F000
UIMAGE_MAGIC = 0x27051956
UIMAGE_HEADER = 64

# start.s, start_first_cpu: `mov x28, #1; str x28, [x16]` is followed by `mov x17, #2; str x17, [x16]` only when built
# with SWITCH_EARLY_TRACE (assembled with clang --target=aarch64).
ENABLE_SEQUENCE = struct.pack('<2I', 0xD280003C, 0xF900021C)
TRACE_SEQUENCE = ENABLE_SEQUENCE + struct.pack('<2I', 0xD2800051, 0xF9000211)


def trace_built(kernel: bytes) -> bool:
    return TRACE_SEQUENCE in kernel


def enable_sequences(kernel: bytes) -> int:
    return kernel.count(ENABLE_SEQUENCE)


def loader_margin(image: bytes) -> dict:
    magic, size = struct.unpack_from('>I8xI', image, 0)[0], struct.unpack_from('>I', image, 12)[0]
    if magic != UIMAGE_MAGIC or len(image) != UIMAGE_HEADER + size:
        raise ValueError('not a legacy U-Boot loader image of the size its header names')
    end = LOAD + size
    return {
        'flat_bytes': size,
        'image_end': end,
        'to_stamps_slot': STAMPS_SLOT - end,
        'to_sd_reference': SD_REFERENCE - end,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('kernel', 'margin'))
    parser.add_argument('path', type=Path)
    parser.add_argument('--stamp', type=Path, help='kernel mode: write this file when the check passes')
    args = parser.parse_args(argv)
    data = args.path.read_bytes()
    if args.mode == 'kernel':
        if not enable_sequences(data):
            print(f'{args.path}: start.s early-rendering enable not found (compressed or not an xnu-10063 kernel)',
                  file=sys.stderr)
            return 1
        if not trace_built(data):
            print(f'{args.path}: no SWITCH_EARLY_TRACE start.s found (early_trace=false or a release kernel)',
                  file=sys.stderr)
            return 1
        if args.stamp:
            args.stamp.write_text('early_trace\n')
        return 0
    result = loader_margin(data)
    for key, value in result.items():
        print(f'{key}: {value:#x} ({value})' if key != 'flat_bytes' else f'{key}: {value}')
    return 0 if result['to_stamps_slot'] >= 0 else 1


if __name__ == '__main__':
    sys.exit(main())
