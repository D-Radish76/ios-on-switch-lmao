"""The Erista's tree: its numbers are the pinned Erista DTB's and the D0 measurements, and only audited nodes exist."""

import hashlib
import re
import struct
import unittest
from pathlib import Path

import native_erista_adt as adt
from native_adt_common import TIMEBASE_HZ

ROOT = Path('.')
DTB = (ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes()
MEMORY_C = (ROOT / 'boot/loader/memory.c').read_text()
SEED = bytes(range(64)) * 4
ROOT_PROFILE = adt.Profile(storage_probe=True, root_boot=True, full_ram=True)
BARE = adt.Profile()
# Drift guards only: they pin what the generator made, they are not evidence about the hardware.
ERISTA_ROOT = '0601b5246267df687678e8d678819940aaeca552eac33eb55a1f244ff8db24bc'
ERISTA_BARE = 'a713091e8c42d1c38e495d99c2ba038d6baaa2364f69aecedf59e44494b71db0'

# Every node the tree may have. A new node is a review item, not a side effect.
AUDITED_NODES = {
    'device-tree',
    'chosen',
    'memory-map',
    'manifest-properties',
    'cpus',
    'cpu0',
    'defaults',
    'options',
    'product',
    'arm-io',
    'intc',
}


def dtb_nodes(blob: bytes) -> dict[str, dict[str, bytes]]:
    _, _, off_struct, off_strings, *_ = struct.unpack('>10I', blob[:40])
    nodes: dict[str, dict[str, bytes]] = {}
    path: list[str] = []
    pos = off_struct
    while True:
        (token,) = struct.unpack('>I', blob[pos : pos + 4])
        pos += 4
        if token == 1:
            end = blob.index(b'\0', pos)
            path.append(blob[pos:end].decode())
            pos = (end + 4) & ~3
            nodes['/'.join(path) or '/'] = {}
        elif token == 2:
            path.pop()
        elif token == 3:
            length, name_offset = struct.unpack('>II', blob[pos : pos + 8])
            pos += 8
            name_end = blob.index(b'\0', off_strings + name_offset)
            nodes['/'.join(path) or '/'][blob[off_strings + name_offset : name_end].decode()] = blob[pos : pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 9:
            return nodes


def adt_nodes(blob: bytes) -> list[tuple[str, dict[str, bytes], int]]:
    """(name, properties, depth) of every node in an Apple DT, in order."""
    found: list[tuple[str, dict[str, bytes], int]] = []

    def walk(pos: int, depth: int) -> int:
        count, children = struct.unpack_from('<II', blob, pos)
        pos += 8
        props: dict[str, bytes] = {}
        for _ in range(count):
            name, length = struct.unpack_from('<32sI', blob, pos)
            length &= 0x7FFFFFFF
            pos += 36
            props[name.rstrip(b'\0').decode()] = blob[pos : pos + length]
            pos += (length + 3) & ~3
        found.append((props['name'].rstrip(b'\0').decode(), props, depth))
        for _ in range(children):
            pos = walk(pos, depth + 1)
        return pos

    end = walk(0, 0)
    assert end == len(blob)
    return found


def cells(value: bytes) -> list[int]:
    return list(struct.unpack(f'>{len(value) // 4}I', value))


def strings(value: bytes) -> tuple[str, ...]:
    return tuple(value.rstrip(b'\0').decode().split('\0'))


DT = dtb_nodes(DTB)
TREE = adt.generate_adt(SEED, ROOT_PROFILE)
BY_NAME = {name: props for name, props, _ in adt_nodes(TREE)}


class Identity(unittest.TestCase):
    def test_the_root_is_the_pinned_dtbs_root(self):
        self.assertEqual(strings(DT['/']['model']), (adt.IDENTITY.model,))
        self.assertEqual(strings(DT['/']['compatible']), adt.IDENTITY.compatible)

    def test_the_tree_carries_that_root_and_the_t210_soc(self):
        _, root, _ = adt_nodes(TREE)[0]
        self.assertEqual(strings(root['model']), (adt.IDENTITY.model,))
        self.assertEqual(strings(root['compatible']), adt.IDENTITY.compatible)
        self.assertEqual(strings(BY_NAME['arm-io']['device_type']), ('nvidia,tegra210',))

    def test_nothing_of_the_oled_is_in_it(self):
        for text in (b'OLED', b'aula', b'fric', b'tegra210b01'):
            self.assertNotIn(text, TREE)


class Interrupts(unittest.TestCase):
    def test_the_gic_is_the_dtbs(self):
        gic = DT['/interrupt-controller']
        self.assertEqual(strings(gic['compatible']), ('arm,cortex-a15-gic',))
        gicd, gicd_size, gicc, gicc_size = cells(gic['reg'])[1::2]
        self.assertEqual((gicd, gicd_size), adt.FACTS.gicd)
        self.assertEqual((gicc, gicc_size), adt.FACTS.gicc)

    def test_the_arm_io_bus_spans_exactly_the_two_frames(self):
        gicd, _ = adt.FACTS.gicd
        gicc, gicc_size = adt.FACTS.gicc
        self.assertEqual(struct.unpack('<3Q', BY_NAME['arm-io']['ranges']), (0, gicd, gicc + gicc_size - gicd))
        reg = struct.unpack('<4Q', BY_NAME['intc']['reg'])
        self.assertEqual(reg, (0, adt.FACTS.gicd[1], gicc - gicd, gicc_size))
        self.assertEqual(strings(BY_NAME['intc']['compatible']), ('arm,cortex-a15-gic',))

    def test_the_timebase_is_the_dtbs_timer_and_the_measured_counter(self):
        self.assertEqual(cells(DT['/timer']['clock-frequency']), [TIMEBASE_HZ])
        self.assertEqual(struct.unpack('<I', BY_NAME['cpu0']['timebase-frequency']), (19_200_000,))


class Cpu(unittest.TestCase):
    def test_one_a57_with_the_dtbs_affinity_and_no_psci(self):
        cpus = [name for name, _, _ in adt_nodes(TREE) if name.startswith('cpu') and name != 'cpus']
        self.assertEqual(cpus, ['cpu0'])
        self.assertEqual(cells(DT['/cpus/cpu@0']['reg'])[-1], struct.unpack('<I', BY_NAME['cpu0']['reg'])[0])
        self.assertIn('arm,cortex-a57', strings(DT['/cpus/cpu@0']['compatible']))
        self.assertEqual(strings(BY_NAME['cpu0']['compatible']), ('arm,cortex-a57',))
        self.assertEqual(strings(BY_NAME['cpu0']['state']), ('running',))
        self.assertNotIn('psci', BY_NAME)

    def test_the_dtb_has_four_cores_and_the_tree_does_not_claim_them(self):
        self.assertEqual(len([p for p in DT if re.fullmatch(r'/cpus/cpu@\d', p)]), 4)

    def test_a_multi_core_profile_is_refused(self):
        with self.assertRaises(ValueError):
            adt.generate_adt(SEED, adt.Profile(storage_probe=True, root_boot=True, full_ram=True, native_smp=True))


class Memory(unittest.TestCase):
    BANKS = ((0x80000000, 0x7FD00000), (0x100000000, 0x80000000))

    def test_the_banks_are_the_ones_the_loader_checks(self):
        expected = re.search(r'expected\[4\] = \{([^}]*)\}', MEMORY_C)
        assert expected is not None
        values = [int(v.rstrip('ULl'), 16) for v in re.findall(r'0x[0-9a-fA-F]+U?LL', expected.group(1))]
        self.assertEqual(values, [v for bank in self.BANKS for v in bank])

    def test_the_tree_window_holds_both_banks(self):
        chosen = BY_NAME['chosen']
        base = struct.unpack('<Q', chosen['dram-base'])[0]
        size = struct.unpack('<Q', chosen['dram-size'])[0]
        self.assertEqual((base, size), (adt.FACTS.dram_base, adt.FACTS.dram_size))
        for start, length in self.BANKS:
            self.assertGreaterEqual(start, base)
            self.assertLessEqual(start + length, base + size)


class Nodes(unittest.TestCase):
    def test_only_audited_nodes_exist(self):
        self.assertEqual({name for name, _, _ in adt_nodes(TREE)} - AUDITED_NODES, set())
        self.assertEqual({name for name, _, _ in adt_nodes(adt.generate_adt(SEED, BARE))} - AUDITED_NODES, set())

    def test_no_peripheral_is_described(self):
        names = {name for name, _, _ in adt_nodes(TREE)}
        for peripheral in ('wlan', 'bluetooth', 'usb-device', 'usb-xhci', 'buttons', 'backlight', 'se', 'se2', 'sdmmc'):
            self.assertNotIn(peripheral, names)

    def test_no_property_names_a_board_part(self):
        for _, props, _ in adt_nodes(TREE):
            for key in props:
                self.assertNotRegex(key, r'tap|trim|panel|touch|pmic|regulator|backlight')


class Regression(unittest.TestCase):
    def test_the_tree_is_deterministic(self):
        self.assertEqual(adt.generate_adt(SEED, ROOT_PROFILE), TREE)

    def test_the_seed_is_checked(self):
        for seed in (b'', bytes(63), bytes(257)):
            with self.assertRaises(ValueError):
                adt.generate_adt(seed, BARE)

    def test_the_trees_do_not_drift(self):
        self.assertEqual(hashlib.sha256(TREE).hexdigest(), ERISTA_ROOT)
        self.assertEqual(hashlib.sha256(adt.generate_adt(SEED, BARE)).hexdigest(), ERISTA_BARE)


if __name__ == '__main__':
    unittest.main()
