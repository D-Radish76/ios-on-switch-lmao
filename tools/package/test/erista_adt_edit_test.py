"""The Erista's tree through adt_edit.py, as the package would run it without --smp, for the selected IPSW kind.

Run it under --config=ios17 and --config=ios27: //ipsw:device_tree is Apple's tree of that kind's iPad.
"""

import importlib.util
import struct
import unittest
from pathlib import Path
from types import ModuleType

import adt_check
import native_erista_adt
import native_oled_adt
from python.runfiles import runfiles

SEED = bytes(range(64)) * 4
PROFILE = native_oled_adt.Profile(storage_probe=True, root_boot=True, full_ram=True)


def load_adt_edit() -> ModuleType:
    path = runfiles.Create().Rlocation('_main/tools/package/adt_edit.py')
    spec = importlib.util.spec_from_file_location('adt_edit', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def apple_tree() -> bytes:
    return Path(runfiles.Create().Rlocation('_main/ipsw/device_tree.adt')).read_bytes()


def edited(adt_edit: ModuleType, base: bytes, smp: bool) -> bytes:
    return adt_edit.with_ipad_identity(adt_edit.ipad_adt(base, smp=smp), apple_tree())


class EristaThroughTheEdit(unittest.TestCase):
    def test_one_core_stays_one_core_and_passes_the_structure_check(self):
        adt_edit = load_adt_edit()
        tree = edited(adt_edit, native_erista_adt.generate_adt(SEED, PROFILE), smp=False)
        self.assertEqual(adt_check.check(tree, 1), [])

    def test_the_edit_adds_no_second_core_or_psci(self):
        adt_edit = load_adt_edit()
        tree = edited(adt_edit, native_erista_adt.generate_adt(SEED, PROFILE), smp=False)
        parsed = adt_check.parse(tree)[0]
        self.assertIsNone(adt_check.find(parsed, '/psci'))
        self.assertEqual([adt_check.node_name(c) for c in adt_check.find(parsed, '/cpus')[1]], ['cpu0'])

    def test_the_erista_keeps_its_own_board_entry_after_the_ipads(self):
        adt_edit = load_adt_edit()
        tree = adt_check.parse(edited(adt_edit, native_erista_adt.generate_adt(SEED, PROFILE), smp=False))[0]
        compatible = tree[0]['compatible'].rstrip(b'\0').split(b'\0')
        self.assertIn(b'nintendo,icosa', compatible)
        self.assertNotIn(b'nintendo,aula', compatible)
        self.assertEqual(compatible[-1], b'nvidia,tegra210')

    def test_no_oled_string_survives_the_edit(self):
        adt_edit = load_adt_edit()
        tree = edited(adt_edit, native_erista_adt.generate_adt(SEED, PROFILE), smp=False)
        for text in (b'nintendo,aula\0', b'nvidia,fric\0', b'nvidia,tegra210b01\0', b'Nintendo Switch (OLED model)\0'):
            self.assertNotIn(text, tree)

    def test_the_oled_still_edits_to_four_cores(self):
        adt_edit = load_adt_edit()
        tree = edited(adt_edit, native_oled_adt.generate_adt(SEED, PROFILE), smp=True)
        self.assertEqual(adt_check.check(tree, 4), [])
        self.assertEqual(struct.unpack('<I', adt_check.find(adt_check.parse(tree)[0], '/cpus/cpu3')[0]['reg']), (3,))


if __name__ == '__main__':
    unittest.main()
