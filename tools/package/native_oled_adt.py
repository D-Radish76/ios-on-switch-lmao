#!/usr/bin/env python3
"""The Apple DeviceTree of the native OLED board (generate_adt).

//boot:adt's full-RAM root-boot profile, which the package then gives its iPad identity (adt_edit.py).
The Erista has its own (native_erista_adt.py); the two share only native_adt_common.py's mechanism.
"""

from __future__ import annotations

from native_adt_common import (
    MANAGED_BASE,
    MANAGED_SIZE,
    MANAGED_USER_BASE,
    ROOT_MATCHING,
    ROOT_VOLUME_UUID,
    SEED_BYTES,
    TIMEBASE_HZ,
    BoardFacts,
    Identity,
    Profile,
    build_tree,
    node,
    property_bytes,
    root_matching,
)

__all__ = [
    'COMPATIBLE',
    'FACTS',
    'MANAGED_BASE',
    'MANAGED_SIZE',
    'MANAGED_USER_BASE',
    'MODEL',
    'ROOT_MATCHING',
    'ROOT_VOLUME_UUID',
    'SEED_BYTES',
    'TIMEBASE_HZ',
    'Identity',
    'Profile',
    'generate_adt',
    'node',
    'property_bytes',
    'root_matching',
]

MODEL = 'Nintendo Switch (OLED model)'
COMPATIBLE = ('nvidia,fric', 'nintendo,aula', 'nintendo,nx', 'nvidia,tegra210b01')
FACTS = BoardFacts(
    identity=Identity(MODEL, COMPATIBLE),
    dram_base=0x80000000,
    dram_size=4 << 30,
    gicd=(0x50041000, 0x1000),
    gicc=(0x50042000, 0x100),
)


def generate_adt(seed: bytes | None, profile: Profile) -> bytes:
    return build_tree(FACTS, seed, profile)
