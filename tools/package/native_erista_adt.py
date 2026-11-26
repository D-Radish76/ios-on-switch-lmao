#!/usr/bin/env python3
"""The Apple DeviceTree of the native Erista board (Nintendo Switch 2017, T210), one CPU.

Separate from the OLED's (native_oled_adt.py): every number here is the Erista's own, with the evidence beside it.
The tree is the fixed minimal description, so nothing of the Erista's DTB is copied: its panel, touch, PMIC, SDMMC
tuning and security engine are not described, because no driver of this tree reads them from the ADT
(ERISTA_ADT.md). The tree is not booted: the Erista row stays `boot 0`.
"""

from __future__ import annotations

from native_adt_common import SEED_BYTES, BoardFacts, Identity, Profile, build_tree

__all__ = ['FACTS', 'IDENTITY', 'SEED_BYTES', 'Profile', 'generate_adt']

# The root of bazel/pinned/firmware/erista-sku0.dtb; native_erista_adt_test.py reads it from there.
IDENTITY = Identity('Nintendo Switch (2017)', ('nvidia,odin', 'nintendo,icosa', 'nintendo,nx', 'nvidia,tegra210'))

# dram: the two banks D0 read (0x80000000 + 0x7FD00000, 0x100000000 + 0x80000000) lie inside this window, which is the
#   OLED's too; the loader's ledger (memory.c) holds the exact banks.
# gic: GICD and GICC bases of the DTB's /interrupt-controller (reg 0x50041000/0x1000, 0x50042000/0x100) and the GIC-400
#   words D0 read there. The CPU interface size is what the DTB lists, not the 0x2000 of a full GIC-400 frame.
FACTS = BoardFacts(
    identity=IDENTITY,
    dram_base=0x80000000,
    dram_size=4 << 30,
    gicd=(0x50041000, 0x1000),
    gicc=(0x50042000, 0x100),
)


def generate_adt(seed: bytes | None, profile: Profile) -> bytes:
    """The Erista runs one CPU until its secondary-core start (PSCI CPU_ON on this BL31) has been measured."""
    if profile.native_smp:
        raise ValueError('the Erista tree is single core: PSCI CPU_ON has not been run on it')
    return build_tree(FACTS, seed, profile)
