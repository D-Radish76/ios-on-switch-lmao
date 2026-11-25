#!/usr/bin/env python3
"""The read only SD card diagnostic (D1-pre): two hekate entries that print what U-Boot says about the SD card.

    diag_sd.py build --frame F --page1 P1 --page2 P2 --boards SwitchBoard.h --bl31 BL31 --bl33 BL33 --out DIR
                     [--dwell S]

Page 1 is `iOS Diag SD`: `devnum`, `mmc list` and `part list mmc 1`, which reaches the card the way the normal script's
`read mmc 1` does (U-Boot's own initialization, not forced). Page 2 is `iOS Diag SD Info`: `mmc dev 1` and `mmc info`.
`mmc dev` starts the card again, so page 2 is not the path the exporter step takes; it is separate for that reason.
Neither carries a loader, exporter, DTB or kernel window, and no other entry's files are touched.
"""

import argparse
from pathlib import Path

from diag_boot import DWELL, diag_bl33, diag_u_boot
from diag_memory import Page, ini, memory_script

PAGES = (
    Page(1, 'ios-diag-sd', 'iOS Diag SD', 'iDiagS'),
    Page(2, 'ios-diag-sd-info', 'iOS Diag SD Info', 'iDiagSI'),
)


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
