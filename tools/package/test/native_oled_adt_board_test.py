"""generate_adt takes the root identity and the SoC from the board, and the OLED's tree stays byte for byte what it was."""

import hashlib
import unittest

import native_oled_adt as adt

SEED = bytes(range(64)) * 4
ROOT = adt.Profile(storage_probe=True, root_boot=True, full_ram=True)
BARE = adt.Profile()

# SHA-256 of the OLED's tree for SEED before the board parameter existed.
OLED_ROOT = '4c008dbf82aace03a8457e6e5eaceee93dc61d30398db5e1801a55c3cca69c2a'
OLED_BARE = 'ef73246b54ab09c8649a21c331b01cdc4c9f24e70c72f9744bca07dbadb4f0f0'


def sha(data):
    return hashlib.sha256(data).hexdigest()


class BoardAdtTest(unittest.TestCase):
    def test_the_oled_tree_did_not_change(self):
        self.assertEqual(sha(adt.generate_adt(SEED, ROOT)), OLED_ROOT)
        self.assertEqual(sha(adt.generate_adt(SEED, BARE)), OLED_BARE)
        self.assertEqual(adt.generate_adt(SEED, ROOT, board='oled'), adt.generate_adt(SEED, ROOT))

    def test_the_erista_tree_names_the_erista_and_nothing_of_the_oled(self):
        tree = adt.generate_adt(SEED, ROOT, board='erista')
        for text in (b'Nintendo Switch (2017)\0', b'nintendo,icosa\0', b'nvidia,odin\0', b'nvidia,tegra210\0'):
            self.assertIn(text, tree)
        for text in (b'OLED', b'aula', b'fric', b'tegra210b01'):
            self.assertNotIn(text, tree)

    def test_the_arm_io_bus_names_the_boards_soc(self):
        self.assertIn(b'nvidia,tegra210b01\0', adt.generate_adt(SEED, BARE, board='oled'))
        self.assertNotIn(b'nvidia,tegra210b01\0', adt.generate_adt(SEED, BARE, board='erista'))

    def test_an_unknown_board_is_refused(self):
        for board in ('', 'OLED', 'unknown', 'lite'):
            with self.assertRaises(ValueError):
                adt.generate_adt(SEED, BARE, board=board)


if __name__ == '__main__':
    unittest.main()
