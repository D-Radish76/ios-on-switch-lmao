#!/usr/bin/env python3
"""The read only memory controller diagnostic: its own hekate entry, its own bl33.bin, no loader and no kernel.

    diag_boot.py build --template T --boards SwitchBoard.h --loader-src loader.c --bl31 BL31 --bl33 BL33 --out DIR
    diag_boot.py compare LOG --loader-src loader.c        what was read, next to what native_gate expects
    diag_boot.py adopt LOG --loader-src loader.c --source TEXT --out FIXTURE
                                                          a measured log as a fixture, with its provenance

`build` writes DIR/bootloader/ini/ios-diag.ini, DIR/switchroot/ios-diag/bl31.bin and bl33.bin. The normal entry's files
are not touched and `boot` in the board table stays as it is.
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

from bl33 import bl33_patch
from boot_script import boards, script_image
from loader_abi import BL33_BASE, SCRIPT_SLOT

IDENTITY_REGISTERS = (0x70000804, 0x7000F9D8)  # HIDREV and FUSE_RESERVED_ODM4: what native_gate's board detection reads
# Seconds the result stays on screen before the reset. The screen is the only way to read it and nothing cools the
# console meanwhile, so it is as short as a photograph allows.
DWELL = 15
STEP = 5

# What U-Boot does when its script does not run, in the diagnostic's image only:
#   * extlinux is not looked for at all, whatever hekate's environment says or fails to say, so no kernel is booted from
#     the SD card's FAT.
#   * distro_bootcmd ends in `reset` after ten seconds, which would send the console back to hekate in a loop with
#     nothing on the screen; it says so, waits the same time as the script does and resets once.
DIAG_ENV_EDITS = {
    (
        b'scan_dev_for_extlinux=if test -e ${devtype} ${devnum}:${distro_bootpart} ${prefix}extlinux/extlinux.conf; '
        b'then echo Found ${prefix}extlinux/extlinux.conf; run boot_extlinux; echoe SCRIPT FAILED: continuing...; fi'
    ): b'scan_dev_for_extlinux=true',
}
DISTRO_BOOTCMD = (
    b'distro_bootcmd=for target in ${boot_targets}; do run bootcmd_${target}; done;'
    b'echoe Failed to load boot device or script!;echoe Rebooting in 10s..;sleep 10; reset;'
)
DISTRO_BOOTCMD_DIAG = (
    b'distro_bootcmd=for target in ${boot_targets}; do run bootcmd_${target}; done;setenv stdout vidconsole;'
    b'echo DIAGNOSTIC DID NOT RUN, NO BOOT. Reset in %d s, expected to land in hekate, not verified;sleep %d;reset;'
    b'while true; do sleep 1; done;'
)


def dwell_steps(dwell: int) -> str:
    if dwell % STEP or not 10 <= dwell <= 60:
        raise SystemExit(f'the wait must be a multiple of {STEP} s from 10 to 60, not {dwell}')
    return ' '.join(str(left) for left in range(dwell - STEP, -1, -STEP))


def diag_env_edits(dwell: int) -> dict[bytes, bytes]:
    dwell_steps(dwell)
    return {**DIAG_ENV_EDITS, DISTRO_BOOTCMD: DISTRO_BOOTCMD_DIAG % (dwell, dwell)}


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


def carveout_table(carveout_src: str) -> list[tuple[int, int]]:
    """(address, OLED value) of native_gate's registers in boot/loader/carveout.h, in its order."""

    def array(name: str) -> list[int]:
        body = re.search(rf'{name}\[SWITCH_CARVEOUT_REGISTERS\] = \{{(.*?)\}};', carveout_src, re.DOTALL)
        if not body:
            raise SystemExit(f'carveout.h has no {name}')
        return [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body.group(1))]

    base = re.search(r'SWITCH_CARVEOUT_MC_BASE (0x[0-9a-fA-F]+)u', carveout_src)
    offsets, values = array('switch_carveout_offset'), array('switch_carveout_OLED')
    if not base or len(offsets) != len(values) or not offsets:
        raise SystemExit('carveout.h has a register row diag_boot.py cannot read')
    return [(int(base.group(1), 16) + o, v) for o, v in zip(offsets, values, strict=True)]


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


def read_step(address: int) -> str:
    """One read that is only tried while every earlier one worked, and counted only if md.l reports success.

    md.l reports a bad argument, not a bus fault: a fault stops U-Boot itself, so the count never reaches its total.
    """
    return '\n'.join(
        [
            '    if test "${diag_status}" = "READY"; then',
            f'        if md.l {address:#010x} 1; then',
            '            setexpr diag_reads ${diag_reads} + 1',
            '        else',
            f'            setenv diag_status READ_FAILED_{address:08x}',
            '        fi',
            '    fi',
        ]
    )


def diag_script(template: str, header: str, loader_src: str, dwell: int = DWELL) -> str:
    addresses = read_addresses(loader_src)
    reads = '\n'.join(read_step(a) for a in addresses)
    for marker in ('@BOARD_CHECK@', '@READS@'):
        if template.count(marker) != 1:
            raise SystemExit(f'the diagnostic template needs {marker} exactly once')
    text = (
        template.replace('@BOARD_CHECK@', board_check(header))
        .replace('@READS@', reads)
        .replace('@COUNT@', str(len(addresses)))
        .replace('@COUNT_HEX@', f'{len(addresses):x}')
        .replace('@DWELL_STEPS@', dwell_steps(dwell))
        .replace('@DWELL@', str(dwell))
        .replace('@STEP@', str(STEP))
    )
    if re.search(r'@[A-Z_0-9]+@', text):
        raise SystemExit('the diagnostic template has a value this tool does not fill')
    return text


def diag_u_boot(bl33: bytes, dwell: int = DWELL) -> bytes:
    return bl33_patch(bl33, diag_env_edits(dwell))


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
    script = diag_script(args.template.read_text(), args.boards.read_text(), loader_src, args.dwell)
    u_boot = diag_u_boot(args.bl33.read_bytes(), args.dwell)
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


def adopt(raw: bytes, loader_src: str, source: str) -> str:
    """Return a measured log as a fixture: a header saying where it came from, then the log itself.

    Refused unless it has every register the script reads, each once, in the script's order, so a log that is not the
    whole run never becomes the reference. Only the line endings are normalized.
    """
    log = raw.decode('utf-8').replace('\r\n', '\n').replace('\r', '\n')
    found = [int(a, 16) for a in re.findall(r'^\s*([0-9a-f]{8}):\s+[0-9a-f]{8}\b', log, re.MULTILINE | re.IGNORECASE)]
    if found != read_addresses(loader_src):
        raise SystemExit(
            f"the log has {len(found)} register lines, not the script's {len(read_addresses(loader_src))} in order"
        )
    header = [
        f'# {source}',
        f'# Copied from the measured log, {len(raw)} bytes, sha256 {hashlib.sha256(raw).hexdigest()}.',
        '# Only the line endings differ.',
    ]
    return '\n'.join(header) + '\n' + log.rstrip('\n') + '\n'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    p = step.add_parser('build')
    for name in ('template', 'boards', 'loader-src', 'bl31', 'bl33', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--dwell', type=int, default=DWELL, help=f'seconds on screen before the reset (default {DWELL})')
    p = step.add_parser('compare')
    p.add_argument('log', type=Path)
    p.add_argument('--loader-src', required=True, type=Path)
    p = step.add_parser('adopt')
    p.add_argument('log', type=Path)
    p.add_argument('--loader-src', required=True, type=Path)
    p.add_argument('--source', required=True, help='where and when it was measured')
    p.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.step == 'build':
        build(args)
    elif args.step == 'adopt':
        args.out.write_text(adopt(args.log.read_bytes(), args.loader_src.read_text(), args.source))
    else:
        print('\n'.join(compare(args.log.read_text(), args.loader_src.read_text())))


if __name__ == '__main__':
    sys.exit(main())
