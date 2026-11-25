#!/usr/bin/env python3
"""The D0 loader diagnostic: its loader, its own bl33.bin and its own hekate entry. No kernel window, no exporter.

    diag_loader.py link --ld-lld P --objcopy P --objdump P --linker-script LD --map OUT --disassembly OUT --report OUT
                        --elf OUT --out LOADER.UBOOT OBJ...
    diag_loader.py build --template T --boards SwitchBoard.h --board NAME --loader L --dtb D --bl31 BL31 --bl33 BL33
                         --out DIR [--dwell S]

`link` links the objects, refuses an image the inspection (diag_loader_inspect.py) does not pass and wraps the raw image
for bootm. `build` writes DIR/bootloader/ini/ios-diag-loader.ini and DIR/switchroot/ios-diag-loader/{bl31.bin,
bl33.bin, script.txt}. The normal entry's files are not touched and `boot` in the board table stays as it is.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from bl33 import bl33_patch
from boot_script import boards, script_image
from diag_boot import DWELL, STEP, diag_env_edits, dwell_steps
from diag_loader_inspect import inspect_disassembly, inspect_map, report
from loader_abi import BL33_BASE, BOOT_ARGS_PAGE, DTB_SLOT, LOADER_SLOT, SCRIPT_SLOT, SD_REFERENCE, STAGE_HEADER
from loader_link import validate_elf
from uimage import LOAD, Header, legacy_image

ENTRY = 'ios-diag-loader'
INI = '\n'.join(
    [
        '[iOS Diag Loader]',
        'l4t=1',
        f'boot_prefixes=/switchroot/{ENTRY}/',
        'id=iDiagL',
        'icon=bootloader/res/icon_ios_hue.bmp',
        'bootwait=0',
        'bootcmd_mmc0=true',
        'scan_dev_for_extlinux=true',
        '',
    ]
)
MAX_IMAGE_END = 0xA001EF00
DRAM = (0x80000000, 0xFFD00000)
FDT_COPY = (0x8D000000, 0x8D000000 + 0x30000)
FRAMEBUFFER = (0xF5A00000, 0xF5A00000 + 0x400000)


def board_gate(header: str, wanted: str) -> str:
    """READY only for the named row of the board table; its `boot` field is not consulted and not changed."""
    rows = [row for row in boards(header) if row[0] == wanted]
    if len(rows) != 1:
        raise SystemExit(f'the board table has no row named {wanted}')
    name, t210b01, sku, _ = rows[0]
    return '\n'.join(
        [
            f'    # {name}',
            f'    if test "${{t210b01}}" = "{t210b01}" -a "${{sku}}" = "{sku}"; then',
            '        setenv handoff_status READY',
            '    fi',
        ]
    )


def diag_script(template: str, header: str, board: str, dtb_bytes: int, dwell: int = DWELL) -> str:
    values = {
        'BOARD_GATE': board_gate(header, board),
        'DTB': hex(DTB_SLOT[0]),
        'DTB_BYTES': hex(dtb_bytes),
        'LOADER': hex(LOADER_SLOT[0]),
        'DWELL_STEPS': dwell_steps(dwell),
        'DWELL': str(dwell),
        'STEP': str(STEP),
    }
    for name, value in values.items():
        if f'@{name}@' not in template:
            raise SystemExit(f'the D0 script template has no @{name}@')
        template = template.replace(f'@{name}@', value)
    if re.search(r'@[A-Z_0-9]+@', template):
        raise SystemExit('the D0 script template has a value this tool does not fill')
    return template


def diag_loader_bl33(patched_u_boot: bytes, script: str, loader: bytes, dtb: bytes) -> bytes:
    """U-Boot, then the script, loader and DTB in their slots, and nothing else: no exporter, no kernel window."""
    slots = [(SCRIPT_SLOT, script_image(script)), (LOADER_SLOT, loader), (DTB_SLOT, dtb)]
    floor = min(slot[0] - BL33_BASE for slot, _ in slots)
    end_ofs = int.from_bytes(patched_u_boot[0x10:0x18], 'little')
    bss_end_ofs = int.from_bytes(patched_u_boot[0x20:0x28], 'little')
    if not 0 < end_ofs <= len(patched_u_boot) <= floor or not 0 < bss_end_ofs <= floor:
        raise SystemExit('U-Boot reaches the first slot')
    image = bytearray(patched_u_boot.ljust(floor, b'\0'))
    for (address, capacity), content in slots:
        if not content or len(content) > capacity:
            raise SystemExit(f'{len(content)} bytes for the {capacity:#x} byte slot at {address:#x}')
        at = address - BL33_BASE
        image = image.ljust(at, b'\0')
        image[at : at + len(content)] = content
    if len(image) > STAGE_HEADER - BL33_BASE:
        raise SystemExit('the D0 image reaches the staged window area')
    return bytes(image)


def check_destination(loader_bytes: int, bl33_bytes: int) -> None:
    """Refuse an image whose `bootm loados` would write over something this package itself places.

    U-Boot's script cannot list the live FDT's reservations, so the part of the check that can be made is made
    here, from the addresses the package fixes. The loader's own FDT walk reports the rest, after the fact.
    """
    load = (LOAD, LOAD + loader_bytes)
    if not (DRAM[0] <= load[0] and load[1] <= DRAM[1]):
        raise SystemExit(f'the loader at {load[0]:#x} is outside DRAM')
    claimed = {
        'bl33.bin': (BL33_BASE, BL33_BASE + bl33_bytes),
        'the DTB copy': FDT_COPY,
        'the framebuffer': FRAMEBUFFER,
        'the boot arguments page': (BOOT_ARGS_PAGE, BOOT_ARGS_PAGE + 0x1000),
        'the SD reference page': (SD_REFERENCE, SD_REFERENCE + 0x1000),
    }
    for name, (start, end) in claimed.items():
        if load[0] < end and start < load[1]:
            raise SystemExit(f'bootm loados would write {load[0]:#x}..{load[1]:#x} over {name} ({start:#x}..{end:#x})')
    if load[1] > MAX_IMAGE_END:
        raise SystemExit(f'the loader reaches {load[1]:#x}, past {MAX_IMAGE_END:#x}')


def link(args: argparse.Namespace) -> None:
    with tempfile.TemporaryDirectory() as scratch:
        flat = Path(scratch) / 'loader.bin'
        linked = subprocess.run(
            [args.ld_lld, '-T', args.linker_script, f'-Map={args.map}', '-o', args.elf, *args.objects],
            capture_output=True,
            text=True,
            check=False,
        )
        if linked.returncode:
            raise SystemExit(linked.stdout + linked.stderr)
        footprint = validate_elf(args.elf.read_bytes())['footprint_end']
        if footprint > MAX_IMAGE_END:
            raise SystemExit(f'the D0 image ends at {footprint:#x}, past {MAX_IMAGE_END:#x}')
        subprocess.run([args.objcopy, '-O', 'binary', args.elf, flat], check=True)
        disassembly = subprocess.run(
            [args.objdump, '-d', '--no-show-raw-insn', args.elf], check=True, capture_output=True, text=True
        ).stdout
        args.disassembly.write_text(disassembly)
        problems = inspect_map(args.map.read_text()) + inspect_disassembly(disassembly)
        if problems:
            raise SystemExit('D0 loader inspection failed:\n' + '\n'.join(problems))
        args.report.write_text('D0 loader inspection passed\n' + report() + '\n')
        image = legacy_image(flat.read_bytes(), Header(os_type=5, kind=2, name='D0Loader', load=LOAD, entry=LOAD))
        args.out.write_bytes(image)


def build(args: argparse.Namespace) -> None:
    dtb = args.dtb.read_bytes()
    script = diag_script(args.template.read_text(), args.boards.read_text(), args.board, len(dtb), args.dwell)
    u_boot = bl33_patch(args.bl33.read_bytes(), diag_env_edits(args.dwell))
    switchroot = args.out / 'switchroot' / ENTRY
    switchroot.mkdir(parents=True, exist_ok=True)
    (args.out / 'bootloader' / 'ini').mkdir(parents=True, exist_ok=True)
    (switchroot / 'bl31.bin').write_bytes(args.bl31.read_bytes())
    loader = args.loader.read_bytes()
    image = diag_loader_bl33(u_boot, script, loader, dtb)
    check_destination(len(loader), len(image))
    (switchroot / 'bl33.bin').write_bytes(image)
    (switchroot / 'script.txt').write_text(script)
    (args.out / 'bootloader' / 'ini' / f'{ENTRY}.ini').write_text(INI)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    p = step.add_parser('link')
    for name in ('ld-lld', 'objcopy', 'objdump', 'linker-script', 'map', 'disassembly', 'report', 'elf', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('objects', nargs='+', type=Path)
    p = step.add_parser('build')
    for name in ('template', 'boards', 'loader', 'dtb', 'bl31', 'bl33', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--board', default='erista')
    p.add_argument('--dwell', type=int, default=DWELL)
    args = parser.parse_args()
    link(args) if args.step == 'link' else build(args)


if __name__ == '__main__':
    sys.exit(main())
