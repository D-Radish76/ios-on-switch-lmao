#!/usr/bin/env python3
"""Derives the board table one package boots a row from, while kits/SwitchBoard/SwitchBoard.h keeps that row at boot 0.

    board_header.py derive --board erista --header SwitchBoard.h --out OUT \
        --early-trace --stop-after-vm --single-core --profile debug --trace-stamp early-trace.ok
    board_header.py verify --board erista --header SwitchBoard.h --derived OUT
    board_header.py agree --derived OUT --script boot-script.txt --stamp OK

derive writes a copy of the header whose only difference is that row's boot field, 0 to 1. It refuses unless every
condition of the first-XNU diagnostic is stated on the command line, so a package that cannot state them cannot get a
booting row. The stamp is the one first_xnu_check.py writes after finding the trace in the kernel, and derive refuses
without it. The row's other fields are pinned here: a header that changed them is refused until this file changes too.
"""

import argparse
import re
from pathlib import Path

# board -> (name, t210b01, sku, features): what the header must say before the row may be enabled.
ENABLEABLE = {'erista': ('erista', '0', '0', '0')}

TABLE = re.compile(r'#define SWITCH_BOARDS\(X\)((?:[^\n]*\\\n)*[^\n]*)')
ROW = re.compile(r'X\(\s*(\w+)\s*,\s*"(\w+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([^,\s]+)\s*,')
CLAUSE = re.compile(
    r'# (\w+)\nif test "\$\{t210b01\}" = "(\d+)" -a "\$\{sku\}" = "(\d+)"; then\n\s+setenv handoff_status READY'
)


def refuse(message: str) -> None:
    raise SystemExit(f'board_header: refused: {message}')


def rows(header: str) -> list[dict]:
    table = TABLE.search(header)
    if not table:
        refuse('the header has no SWITCH_BOARDS table')
    found = [
        {
            'id': m[1],
            'name': m[2],
            't210b01': m[3],
            'sku': m[4],
            'boot': m[5],
            'features': m[6],
            'boot_at': table.start(1) + m.start(5),
        }
        for m in ROW.finditer(table.group(1))
    ]
    if not found:
        refuse('SWITCH_BOARDS has no row this tool can read')
    return found


def checked_row(header: str, board: str) -> dict:
    if board not in ENABLEABLE:
        refuse(f'unknown board {board!r}; this tool may enable {sorted(ENABLEABLE)}')
    name, t210b01, sku, features = ENABLEABLE[board]
    matching = [r for r in rows(header) if r['name'] == name]
    if len(matching) != 1:
        refuse(f'the header has {len(matching)} rows named {name!r}, not one')
    row = matching[0]
    if (row['t210b01'], row['sku']) != (t210b01, sku):
        refuse(f'the {name} row says t210b01 {row["t210b01"]} sku {row["sku"]}, not the pinned {t210b01} {sku}')
    if row['features'] != features:
        refuse(f'the {name} row has features {row["features"]}: only a row with none may be enabled')
    if row['boot'] != '0':
        refuse(f'the {name} row already says boot {row["boot"]}: the header is not what this tool was written against')
    return row


def derive(header: str, board: str, *, early_trace: bool, stop_after_vm: bool, single_core: bool, profile: str) -> str:  # noqa: PLR0913
    for ok, reason in (
        (profile == 'debug', f'the {profile} profile'),
        (early_trace, 'no early_trace kernel'),
        (stop_after_vm, 'no stop_after_vm'),
        (single_core, 'more than one core'),
    ):
        if not ok:
            refuse(reason)
    at = checked_row(header, board)['boot_at']
    return header[:at] + '1' + header[at + 1 :]


def trace_stamp(path: Path | None) -> bool:
    return path is not None and path.is_file() and path.read_text() == 'early_trace\n'


def verify(original: str, derived: str, board: str) -> None:
    at = checked_row(original, board)['boot_at']
    if len(original) != len(derived) or original[:at] != derived[:at] or original[at + 1 :] != derived[at + 1 :]:
        refuse('the derived header differs from the original outside the one boot field')
    if derived[at] != '1':
        refuse('the derived header does not set that row to boot 1')


def agree(derived: str, script: str) -> None:
    """U-Boot's script accepts exactly the rows of the header the loader is built from that boot."""
    booting = sorted((r['name'], r['t210b01'], r['sku']) for r in rows(derived) if r['boot'] != '0')
    accepted = sorted(CLAUSE.findall(script))
    if booting != accepted:
        refuse(f'the script accepts {accepted} and the header boots {booting}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('command', choices=('derive', 'verify', 'agree'))
    parser.add_argument('--board')
    parser.add_argument('--header', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--derived', type=Path)
    parser.add_argument('--script', type=Path)
    parser.add_argument('--stamp', type=Path)
    parser.add_argument('--profile', default='')
    parser.add_argument('--trace-stamp', type=Path)
    for flag in ('early-trace', 'stop-after-vm', 'single-core'):
        parser.add_argument(f'--{flag}', action='store_true')
    args = parser.parse_args()
    if args.command == 'derive':
        if not trace_stamp(args.trace_stamp):
            refuse('no early-trace stamp: the kernel was not shown to carry the trace')
        text = derive(
            args.header.read_text(),
            args.board,
            early_trace=args.early_trace,
            stop_after_vm=args.stop_after_vm,
            single_core=args.single_core,
            profile=args.profile,
        )
        args.out.write_text(text)
        verify(args.header.read_text(), args.out.read_text(), args.board)
    elif args.command == 'verify':
        verify(args.header.read_text(), args.derived.read_text(), args.board)
    else:
        agree(args.derived.read_text(), args.script.read_text())
    if args.stamp:
        args.stamp.write_text('ok\n')


if __name__ == '__main__':
    main()
