#!/usr/bin/env python3
"""The read only memory diagnostic (D0-pre): two hekate entries that print what U-Boot says about memory.

    diag_memory.py build --frame F --page1 P1 --page2 P2 --boards SwitchBoard.h --bl31 BL31 --bl33 BL33 --out DIR
                         [--dwell S]

`build` writes, for each page, DIR/bootloader/ini/<entry>.ini and DIR/switchroot/<entry>/{bl31.bin,bl33.bin,script.txt}.
Page 1 is `iOS Diag Memory` (board and bdinfo), page 2 is `iOS Diag Memory FDT` (the control FDT's memory, reserved
memory and reserve map). Neither carries a loader, DTB, exporter or kernel window, and no other entry's files are
touched.
"""

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

from diag_boot import DWELL, STEP, board_check, diag_bl33, diag_u_boot, dwell_steps


@dataclass(frozen=True)
class Page:
    number: int
    entry: str
    title: str
    ident: str


PAGES = (
    Page(1, 'ios-diag-memory', 'iOS Diag Memory', 'iDiagM'),
    Page(2, 'ios-diag-memory-fdt', 'iOS Diag Memory FDT', 'iDiagMF'),
)


def ini(page: Page) -> str:
    return '\n'.join(
        [
            f'[{page.title}]',
            'l4t=1',
            f'boot_prefixes=/switchroot/{page.entry}/',
            f'id={page.ident}',
            'icon=bootloader/res/icon_ios_hue.bmp',
            'bootwait=0',
            'bootcmd_mmc0=true',
            'scan_dev_for_extlinux=true',
            '',
        ]
    )


def memory_script(frame: str, body: str, header: str, page: int, dwell: int = DWELL) -> str:
    for marker in ('@BOARD_CHECK@', '@BODY@'):
        if frame.count(marker) != 1:
            raise SystemExit(f'the memory diagnostic frame needs {marker} exactly once')
    text = (
        frame.replace('@BOARD_CHECK@', board_check(header))
        .replace('@BODY@', body.rstrip('\n'))
        .replace('@PAGE@', str(page))
        .replace('@DWELL_STEPS@', dwell_steps(dwell))
        .replace('@DWELL@', str(dwell))
        .replace('@STEP@', str(STEP))
    )
    if re.search(r'@[A-Z_0-9]+@', text):
        raise SystemExit('the memory diagnostic has a value this tool does not fill')
    return text


def build(args: argparse.Namespace) -> None:
    header = args.boards.read_text()
    frame = args.frame.read_text()
    u_boot = diag_u_boot(args.bl33.read_bytes(), args.dwell)
    bodies = {1: args.page1.read_text(), 2: args.page2.read_text()}
    (args.out / 'bootloader' / 'ini').mkdir(parents=True, exist_ok=True)
    for page in PAGES:
        script = memory_script(frame, bodies[page.number], header, page.number, args.dwell)
        switchroot = args.out / 'switchroot' / page.entry
        switchroot.mkdir(parents=True, exist_ok=True)
        (switchroot / 'bl31.bin').write_bytes(args.bl31.read_bytes())
        (switchroot / 'bl33.bin').write_bytes(diag_bl33(u_boot, script))
        (switchroot / 'script.txt').write_text(script)
        (args.out / 'bootloader' / 'ini' / f'{page.entry}.ini').write_text(ini(page))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    p = step.add_parser('build')
    for name in ('frame', 'page1', 'page2', 'boards', 'bl31', 'bl33', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--dwell', type=int, default=DWELL)
    p.set_defaults(run=build)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
