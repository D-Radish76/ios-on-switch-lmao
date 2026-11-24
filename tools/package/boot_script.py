#!/usr/bin/env python3
"""U-Boot's half of the handoff: boot/u-boot-script.txt with this build's window and slot values.

As text and as the legacy script image bl33.py stages in bl33.bin (loader_abi.SCRIPT_SLOT), where the patched U-Boot
sources it.

    boot_script.py --template T --constants C --dtb DTB --boards kits/SwitchBoard/SwitchBoard.h --text OUT --image OUT
"""

import argparse
import json
import re
import struct
from pathlib import Path

from loader_abi import BOOT_ARGS_BYTES, BOOT_ARGS_PAGE, DTB_SLOT, EXPORTER_SLOT, LOADER_SLOT, STAGE_SOURCE
from uimage import Header, legacy_image


BOARD_ROW = re.compile(r'X\(\s*(\w+)\s*,\s*"(\w+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,')


def boards(header: str) -> list[tuple[str, int, int, bool]]:
    """(name, t210b01, sku, boot) of every row of SWITCH_BOARDS in kits/SwitchBoard/SwitchBoard.h."""
    table = re.search(r'#define SWITCH_BOARDS\(X\)((?:[^\n]*\\\n)*[^\n]*)', header)
    if not table:
        raise SystemExit('SwitchBoard.h has no SWITCH_BOARDS table')
    rows = [(name, int(t210b01), int(sku), boot == '1') for _, name, t210b01, sku, boot in BOARD_ROW.findall(table.group(1))]
    if not rows or len(rows) != table.group(1).count('X('):
        raise SystemExit('SWITCH_BOARDS has a row boot_script.py cannot read')
    return rows


def board_gate(header: str) -> str:
    """U-Boot's board check: only a row with boot 1 makes the handoff READY, everything else is WRONG_BOARD."""
    lines = ['setenv handoff_status WRONG_BOARD']
    for name, t210b01, sku, boot in boards(header):
        if boot:
            lines += [
                f'# {name}',
                f'if test "${{t210b01}}" = "{t210b01}" -a "${{sku}}" = "{sku}"; then',
                '    setenv handoff_status READY',
                'fi',
            ]
    return '\n'.join(lines)


def boot_script(text: str, constants: dict, dtb_bytes: int, boards_header: str) -> str:
    """Fill every @NAME@ of the template.

    The values: the staged window's header and trailer words (boot/loader/stage.h), the slots, the bootargs.txt page
    and the board check (@BOARD_GATE@, from the board table).
    """
    staged = constants['PACKED_BYTES']
    trailer = STAGE_SOURCE + staged
    values = {
        'ARGS_PA': BOOT_ARGS_PAGE,
        'ARGS_BYTES': BOOT_ARGS_BYTES,
        'DTB': DTB_SLOT[0],
        'DTB_BYTES': dtb_bytes,
        'EXPORTER': EXPORTER_SLOT[0],
        'LOADER': LOADER_SLOT[0],
        'STAGE_STREAM': constants['STREAM_BYTES'],
        'STAGE_BYTES': staged,
        'STAGE_ID_LOW': constants['STAGE_ID'] & 0xFFFFFFFF,
        'STAGE_ID_HIGH': constants['STAGE_ID'] >> 32,
        **{f'TRAILER_{i}': trailer + 4 * i for i in range(6)},
    }
    for name, value in values.items():
        if f'@{name}@' not in text:
            raise SystemExit(f'boot script template has no @{name}@')
        text = text.replace(f'@{name}@', hex(value))
    if '@BOARD_GATE@' not in text:
        raise SystemExit('boot script template has no @BOARD_GATE@')
    text = text.replace('@BOARD_GATE@', board_gate(boards_header))
    if re.search(r'@[A-Z_0-9]+@', text):
        raise SystemExit('boot script template has a value this tool does not fill')
    return text


def script_image(script: str) -> bytes:
    """Wrap the script as U-Boot's source command takes it: a legacy script image of one part."""
    data = script.encode('ascii')
    return legacy_image(struct.pack('>II', len(data), 0) + data, Header(os_type=17, kind=6, name='Boot'))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('template', 'constants', 'dtb', 'boards', 'text', 'image'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    script = boot_script(
        args.template.read_text(),
        json.loads(args.constants.read_text()),
        args.dtb.stat().st_size,
        args.boards.read_text(),
    )
    args.text.write_text(script)
    args.image.write_bytes(script_image(script))


if __name__ == '__main__':
    main()
