#!/usr/bin/env python3
"""The kernel's built-in boot-args, baked into the kernel window's boot_args (//boot:package).

    boot_args.py OUT [--smp] [--release] [--stop-after-vm[=SECONDS]]

Every feature is on in code, and what XNU used to be told on the command line comes from its own defaults and the
ADT (adt_edit.py). The loader appends switchroot/ios/bootargs.txt at boot, whose words can only turn something off.
The kernel reads the line as one string of at most 1023 bytes (native_xnu_layout checks it).
"""

import sys
from pathlib import Path

# Apple's AppleKeyStore takes a shared Data volume from this word or from the ADT's /filesystems e-apfs. e-apfs would
# also put apfs.kext in its enhanced mode ("enhanced apfs is enabled"), which has never run on this hardware.
APPLE_KEYSTORE = '-apfs_shared_datavolume'
DEBUG_LINE = APPLE_KEYSTORE
RELEASE_LINE = APPLE_KEYSTORE
# The first-XNU observation resets the console SECONDS after arm_vm_init has returned (diagnostics/switch/SwitchVmDone.c, trace
# builds only). The kernel accepts 1 to 60 and refuses anything else, a repeated word included.
STOP_AFTER_VM = 'switch_stop_after_vm'
DEFAULT_RESET_SECONDS = 15
MAX_RESET_SECONDS = 60


def command_line(smp: bool, release: bool = False, stop_after_vm: bool | int = False) -> str:
    """Return the built-in line for this profile; four cores or one is the ADT's (adt_edit.py --smp), not the line's."""
    del smp
    if stop_after_vm and release:
        raise ValueError('the stop after VM initialisation exists only in debug kernels')
    line = RELEASE_LINE if release else DEBUG_LINE
    if stop_after_vm is False:
        return line
    seconds = DEFAULT_RESET_SECONDS if stop_after_vm is True else stop_after_vm
    if not 1 <= seconds <= MAX_RESET_SECONDS:
        raise ValueError(f'the reset wait is 1 to {MAX_RESET_SECONDS} seconds')
    return f'{line} {STOP_AFTER_VM}={seconds}'


def parse_flags(flags: list[str]) -> tuple[bool, bool, bool | int]:
    names = [f.split('=')[0] for f in flags]
    if not set(names) <= {'--smp', '--release', '--stop-after-vm'} or len(set(names)) != len(names):
        raise SystemExit(__doc__)
    stop: bool | int = False
    for flag in flags:
        if flag.startswith('--stop-after-vm'):
            _, _, value = flag.partition('=')
            if '=' in flag and not (value.isascii() and value.isdigit() and len(value) <= 2):
                raise SystemExit(__doc__)
            stop = int(value) if value else True
    return '--smp' in flags, '--release' in flags, stop


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    Path(sys.argv[1]).write_text(command_line(*parse_flags(sys.argv[2:])) + '\n')
