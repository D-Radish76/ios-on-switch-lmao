#!/usr/bin/env python3
"""The read only memory controller diagnostic: its own hekate entry, its own bl33.bin, no loader and no kernel.

    diag_boot.py build --template T --boards SwitchBoard.h --loader-src loader.c --bl31 BL31 --bl33 BL33 --out DIR
    diag_boot.py compare LOG --loader-src loader.c        what was read, next to what native_gate expects

`build` writes DIR/bootloader/ini/ios-diag.ini, DIR/switchroot/ios-diag/bl31.bin and bl33.bin. The normal entry's files
are not touched and `boot` in the board table stays as it is.
"""

import argparse
import re
import sys
from pathlib import Path

from bl33 import bl33_patch
from boot_script import boards, script_image
from loader_abi import BL33_BASE, SCRIPT_SLOT

IDENTITY_REGISTERS = (0x70000804, 0x7000F9D8)  # HIDREV and FUSE_RESERVED_ODM4: what native_gate's board detection reads
ENTRY = 'ios-diag'
INI = '\n'.join(
    [
        '[iOS Diag]',
        'l4t=1',
        f'boot_prefixes=/switchroot/{ENTRY}/',
        'id=iDiag',
        'icon=bootloader/res/icon_ios_hue.bmp',
        'bootwait=0',
        'bootcmd_mmc0=true',
        'scan_dev_for_extlinux=true',
        '',
    ]
)


def carveout_table(loader_src: str) -> list[tuple[int, int]]:
    """(address, OLED value) of native_gate's `mc` table in boot/loader/loader.c, in its order."""
    gate = re.search(r'static const uint32_t mc\[\]\[2\] = \{(.*?)\n    \};', loader_src, re.DOTALL)
    if not gate:
        raise SystemExit('loader.c has no native_gate mc table')
    pairs = re.findall(r'\{(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+|\d+)\}', gate.group(1))
    if not pairs or len(pairs) != gate.group(1).count('{'):
        raise SystemExit('native_gate has a register row diag_boot.py cannot read')
    return [(int(a, 16), int(v, 0)) for a, v in pairs]


def read_addresses(loader_src: str) -> list[int]:
    return [*IDENTITY_REGISTERS, *(a for a, _ in carveout_table(loader_src))]


def board_check(header: str) -> str:
    """READY for every board in the table, booting or not: the diagnostic only reads."""
    lines = []
    for name, t210b01, sku, _ in boards(header):
        lines += [
            f'# {name}',
            f'if test "${{t210b01}}" = "{t210b01}" -a "${{sku}}" = "{sku}"; then',
            '    setenv diag_status READY',
            'fi',
        ]
    return '\n'.join(lines)


def diag_script(template: str, header: str, loader_src: str) -> str:
    reads = '\n'.join(f'    md.l {a:#010x} 1' for a in read_addresses(loader_src))
    for marker in ('@BOARD_CHECK@', '@READS@'):
        if template.count(marker) != 1:
            raise SystemExit(f'the diagnostic template needs {marker} exactly once')
    text = template.replace('@BOARD_CHECK@', board_check(header)).replace('@READS@', reads)
    if re.search(r'@[A-Z_0-9]+@', text):
        raise SystemExit('the diagnostic template has a value this tool does not fill')
    return text


def diag_bl33(patched_u_boot: bytes, script: str) -> bytes:
    """U-Boot, then its boot script slot, and nothing else: no loader, exporter, DTB or kernel window."""
    image = script_image(script)
    address, capacity = SCRIPT_SLOT
    slot = address - BL33_BASE
    end_ofs = int.from_bytes(patched_u_boot[0x10:0x18], 'little')
    bss_end_ofs = int.from_bytes(patched_u_boot[0x20:0x28], 'little')
    if len(image) > capacity:
        raise SystemExit(f'the diagnostic script image is {len(image)} bytes for a {capacity:#x} byte slot')
    if not 0 < end_ofs <= len(patched_u_boot) <= slot or not 0 < bss_end_ofs <= slot:
        raise SystemExit('U-Boot reaches the boot script slot')
    return patched_u_boot.ljust(slot, b'\0') + image


def build(args: argparse.Namespace) -> None:
    loader_src = args.loader_src.read_text()
    script = diag_script(args.template.read_text(), args.boards.read_text(), loader_src)
    u_boot = bl33_patch(args.bl33.read_bytes())
    switchroot = args.out / 'switchroot' / ENTRY
    switchroot.mkdir(parents=True, exist_ok=True)
    (args.out / 'bootloader' / 'ini').mkdir(parents=True, exist_ok=True)
    (switchroot / 'bl31.bin').write_bytes(args.bl31.read_bytes())
    (switchroot / 'bl33.bin').write_bytes(diag_bl33(u_boot, script))
    (switchroot / 'script.txt').write_text(script)
    (args.out / 'bootloader' / 'ini' / f'{ENTRY}.ini').write_text(INI)


def compare(log: str, loader_src: str) -> list[str]:
    """One line per register native_gate checks: what the screen showed against its OLED value."""
    seen = {
        int(a, 16): int(v, 16)
        for a, v in re.findall(r'^\s*([0-9a-f]{8}):\s+([0-9a-f]{8})\b', log, re.MULTILINE | re.IGNORECASE)
    }
    lines = []
    for address, expected in carveout_table(loader_src):
        value = seen.get(address)
        verdict = 'NOT READ' if value is None else 'same' if value == expected else 'DIFFERENT'
        shown = '--------' if value is None else f'{value:08x}'
        lines.append(f'{address:#010x} {shown} oled {expected:#010x} {verdict}')
    return lines


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    p = step.add_parser('build')
    for name in ('template', 'boards', 'loader-src', 'bl31', 'bl33', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p = step.add_parser('compare')
    p.add_argument('log', type=Path)
    p.add_argument('--loader-src', required=True, type=Path)
    args = parser.parse_args()
    if args.step == 'build':
        build(args)
    else:
        print('\n'.join(compare(args.log.read_text(), args.loader_src.read_text())))


if __name__ == '__main__':
    sys.exit(main())
