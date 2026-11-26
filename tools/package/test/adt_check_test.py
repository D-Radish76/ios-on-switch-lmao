"""adt_check.py accepts the OLED's and the Erista's trees and names what a missing or wrong property breaks."""

import struct
import unittest

import adt_check
import native_erista_adt
import native_oled_adt

SEED = bytes(range(64)) * 4
ROOT = native_oled_adt.Profile(storage_probe=True, root_boot=True, full_ram=True)
SMP = native_oled_adt.Profile(storage_probe=True, root_boot=True, full_ram=True, native_smp=True)


def prop(name: str, value: bytes) -> bytes:
    return name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + bytes(-len(value) % 4)


class Accepts(unittest.TestCase):
    def test_the_erista_tree_is_one_core(self):
        self.assertEqual(adt_check.check(native_erista_adt.generate_adt(SEED, ROOT), 1), [])

    def test_the_oled_trees(self):
        self.assertEqual(adt_check.check(native_oled_adt.generate_adt(SEED, ROOT), 1), [])
        self.assertEqual(adt_check.check(native_oled_adt.generate_adt(SEED, SMP), 4), [])

    def test_the_erista_tree_is_not_four_cores(self):
        problems = adt_check.check(native_erista_adt.generate_adt(SEED, ROOT), 4)
        self.assertTrue(any('not 4 core' in p for p in problems))
        self.assertTrue(any('/psci' in p for p in problems))

    def test_a_four_core_tree_is_not_one_core(self):
        problems = adt_check.check(native_oled_adt.generate_adt(SEED, SMP), 1)
        self.assertTrue(any('not 1 core' in p for p in problems))


class Refuses(unittest.TestCase):
    TREE = native_erista_adt.generate_adt(SEED, ROOT)

    def test_trailing_bytes(self):
        self.assertTrue(any('after the tree' in p for p in adt_check.check(self.TREE + bytes(4), 1)))

    def test_a_truncated_tree(self):
        self.assertTrue(adt_check.check(self.TREE[:-40], 1))

    def test_the_tree_has_every_required_property_at_its_size(self):
        for path, name, size, _ in adt_check.REQUIRED:
            if name is None or size is None:
                continue
            tree = adt_check.parse(self.TREE)[0]
            node = adt_check.find(tree, path)
            self.assertIsNotNone(node, path)
            self.assertEqual(len(node[0][name]), size, f'{path} {name}')

    def test_a_wrong_timebase(self):
        good = prop('timebase-frequency', struct.pack('<I', 19_200_000))
        bad = self.TREE.replace(good, prop('timebase-frequency', struct.pack('<I', 19_200_001)))
        self.assertTrue(any('timebase' in p for p in adt_check.check(bad, 1)))

    def test_a_gic_outside_the_bus_window(self):
        reg = native_erista_adt.generate_adt(SEED, ROOT)
        good = prop('reg', struct.pack('<4Q', 0, 0x1000, 0x1000, 0x100))
        self.assertIn(good, reg)
        bad = reg.replace(good, prop('reg', struct.pack('<4Q', 0, 0x1000, 0x2000, 0x100)))
        self.assertTrue(any('ranges window' in p for p in adt_check.check(bad, 1)))

    def test_a_missing_node(self):
        bad = self.TREE.replace(b'memory-map', b'memory-mop')
        self.assertTrue(any('memory-map' in p for p in adt_check.check(bad, 1)))


if __name__ == '__main__':
    unittest.main()
