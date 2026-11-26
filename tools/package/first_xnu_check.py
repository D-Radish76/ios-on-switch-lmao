"""Checks for the first-XNU observation package, read from the built artifacts.

kernel KERNEL    exits 0 only if the kernel's start.s was built with SWITCH_EARLY_TRACE (the early marks draw).
margin LOADER    prints the loader image's room below U-Boot's stamps slot and the SD reference page.
macho KERNEL     checks segments, protections and entry (macho_check.py); --compare OTHER lists differences.
"""

import argparse
import struct
import sys
from pathlib import Path

import macho_check as macho

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


def run_macho(args) -> int:
    status = 0
    images = []
    for path in [args.path, *([args.compare] if args.compare else [])]:
        try:
            image, errors = macho.check(path, args.base, args.phys_base, args.entry_phys)
        except (macho.MachoError, OSError) as exc:
            print(f'{path}: {exc}', file=sys.stderr)
            return 1
        images.append(image)
        print(path)
        print('\n'.join('  ' + row for row in macho.table(image)))
        print('\n'.join(f'  note: {line}' for line in macho.notes(image)))
        for error in errors:
            print(f'{path}: {error}', file=sys.stderr)
        status |= bool(errors)
    if args.compare:
        lines = macho.differences(*images)
        print('differences:' if lines else 'no difference in segments or entry')
        print('\n'.join('  ' + line for line in lines))
    return status


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('kernel', 'margin', 'macho'))
    parser.add_argument('path', type=Path)
    parser.add_argument('--stamp', type=Path, help='kernel mode: write this file when the check passes')
    parser.add_argument('--base', type=lambda v: int(v, 0), help='macho: the lowest segment must be at this address')
    parser.add_argument('--phys-base', type=lambda v: int(v, 0), default=0xB207C000, help='macho: where that segment goes')
    parser.add_argument('--entry-phys', type=lambda v: int(v, 0), help='macho: the entry must map to this physical address')
    parser.add_argument('--compare', type=Path, help='macho: a second kernel to list differences against (not a failure)')
    args = parser.parse_args(argv)
    if args.mode == 'macho':
        return run_macho(args)
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
