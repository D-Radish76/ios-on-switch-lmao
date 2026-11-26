#!/usr/bin/env python3
"""adt_check.py ADT [--cores N]: structural check of a native board's Apple DeviceTree.

It checks what the kernel and the drivers read at boot, not that the machine will boot. Every required property names
its reader in REQUIRED. Prints one problem per line and exits 1 if there is any.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

CNTFRQ = 19_200_000
NAME_BYTES = 32

# (node path, property, bytes or None for any length, reader)
REQUIRED = [
    ('/', 'model', None, 'switch_board_resolve, MobileGestalt'),
    ('/', 'compatible', None, 'switch_board_resolve (DT witness)'),
    ('/chosen', 'dram-base', 8, 'osfmk/arm/arm_init.c arm_init'),
    ('/chosen', 'dram-size', 8, 'osfmk/arm/arm_init.c arm_init'),
    ('/chosen', 'random-seed', None, 'pexpert/gen/pe_gen.c'),
    ('/chosen', 'switch-sd-reference', 16, 'diagnostics/switch/SwitchSD.c'),
    ('/chosen', 'root-matching', None, 'IOKit root mount'),
    ('/chosen/memory-map', None, None, 'arm_vm_init asserts the node'),
    ('/cpus/cpu0', 'reg', 4, 'ArmCpu.cpp'),
    ('/cpus/cpu0', 'state', None, 'pexpert/arm/pe_identify_machine.c'),
    ('/cpus/cpu0', 'timebase-frequency', 4, 'pexpert/arm/pe_identify_machine.c'),
    ('/arm-io', 'device_type', None, 'ArmDTPlatform'),
    ('/arm-io', 'ranges', 24, 'ArmGIC.cpp, ArmCpu.cpp mapGic'),
    ('/arm-io/intc', 'compatible', None, 'ArmGIC match'),
    ('/arm-io/intc', 'reg', 32, 'ArmGIC.cpp, ArmCpu.cpp mapGic'),
]
Tree = tuple[dict[str, bytes], list]


def parse(data: bytes, pos: int = 0) -> tuple[Tree, int]:
    count, children = struct.unpack_from('<II', data, pos)
    pos += 8
    props: dict[str, bytes] = {}
    for _ in range(count):
        if pos + 36 > len(data):
            raise ValueError('a property runs past the end')
        name = data[pos : pos + NAME_BYTES].split(b'\0', 1)[0].decode()
        size = struct.unpack_from('<I', data, pos + NAME_BYTES)[0] & 0x7FFFFFFF
        if name in props:
            raise ValueError(f'property {name!r} twice in one node')
        if pos + 36 + size > len(data):
            raise ValueError(f'property {name!r} runs past the end')
        props[name] = data[pos + 36 : pos + 36 + size]
        pos += 36 + size + (-size % 4)
    nodes = []
    for _ in range(children):
        node, pos = parse(data, pos)
        nodes.append(node)
    return (props, nodes), pos


def node_name(node: Tree) -> str:
    return node[0].get('name', b'').split(b'\0', 1)[0].decode()


def find(root: Tree, path: str) -> Tree | None:
    node = root
    for part in [p for p in path.split('/') if p]:
        node = next((c for c in node[1] if node_name(c) == part), None)
        if node is None:
            return None
    return node


def check(data: bytes, cores: int) -> list[str]:
    try:
        root, end = parse(data)
    except (ValueError, struct.error) as error:
        return [f'cannot parse: {error}']
    problems = [f'{len(data) - end} bytes after the tree'] if end != len(data) else []
    for path, prop, size, reader in REQUIRED:
        node = find(root, path)
        if node is None:
            problems.append(f'{path} is missing ({reader})')
        elif prop is not None and prop not in node[0]:
            problems.append(f'{path} has no {prop} ({reader})')
        elif prop is not None and size is not None and len(node[0][prop]) != size:
            problems.append(f'{path} {prop} is {len(node[0][prop])} bytes, not {size} ({reader})')
    cpus = find(root, '/cpus')
    names = sorted(node_name(c) for c in cpus[1]) if cpus else []
    if names != [f'cpu{i}' for i in range(cores)]:
        problems.append(f'/cpus has {names}, not {cores} core(s)')
    psci = find(root, '/psci')
    if (psci is None) != (cores == 1):
        problems.append('/psci must exist exactly when there is more than one core')
    elif psci is not None and psci[0].get('method') != b'smc\0':
        problems.append('/psci method is not smc')
    for core in cpus[1] if cpus else []:
        state = core[0].get('state', b'').rstrip(b'\0')
        if state != (b'running' if node_name(core) == 'cpu0' else b'stopped'):
            problems.append(f'/cpus/{node_name(core)} state {state!r}')
        if core[0].get('timebase-frequency') != struct.pack('<I', CNTFRQ):
            problems.append(f'/cpus/{node_name(core)} timebase-frequency is not {CNTFRQ}')
    bus, intc = find(root, '/arm-io'), find(root, '/arm-io/intc')
    if bus and intc and len(bus[0].get('ranges', b'')) == 24 and len(intc[0].get('reg', b'')) == 32:
        _, base, span = struct.unpack('<3Q', bus[0]['ranges'])
        distributor, size, cpu_if, cpu_size = struct.unpack('<4Q', intc[0]['reg'])
        if size < 0x1000 or distributor + size > span or cpu_if + cpu_size > span:
            problems.append('/arm-io/intc reg does not lie inside the /arm-io ranges window')
        if base == 0:
            problems.append('/arm-io ranges has no bus base')
    return problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('adt', type=Path)
    parser.add_argument('--cores', type=int, default=1)
    args = parser.parse_args()
    problems = check(args.adt.read_bytes(), args.cores)
    print('\n'.join(problems) if problems else f'{args.adt}: ADT OK ({args.cores} core(s))')
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
