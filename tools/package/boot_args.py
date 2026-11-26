#!/usr/bin/env python3
"""The kernel's built-in boot-args, baked into the kernel window's boot_args (//boot:package).

    boot_args.py OUT [--smp] [--release] [--stop-after-vm]

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
# The first-XNU observation stops once arm_vm_init has returned (diagnostics/switch/SwitchVmDone.c, trace builds only).
STOP_AFTER_VM = 'switch_stop_after_vm=1'


def command_line(smp: bool, release: bool = False, stop_after_vm: bool = False) -> str:
    """Return the built-in line for this profile; four cores or one is the ADT's (adt_edit.py --smp), not the line's."""
    del smp
    if stop_after_vm and release:
        raise ValueError('the stop after VM initialisation exists only in debug kernels')
    line = RELEASE_LINE if release else DEBUG_LINE
    return f'{line} {STOP_AFTER_VM}' if stop_after_vm else line


if __name__ == '__main__':
    flags = sys.argv[2:]
    if len(sys.argv) < 2 or not set(flags) <= {'--smp', '--release', '--stop-after-vm'} or len(set(flags)) != len(flags):
        raise SystemExit(__doc__)
    Path(sys.argv[1]).write_text(command_line('--smp' in flags, '--release' in flags, '--stop-after-vm' in flags) + '\n')
