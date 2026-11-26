"""The OLED's tree is byte for byte what it was, and nothing of the Erista is in it."""

import hashlib
import unittest

import native_oled_adt as adt

SEED = bytes(range(64)) * 4
ROOT = adt.Profile(storage_probe=True, root_boot=True, full_ram=True)
BARE = adt.Profile()

# SHA-256 of the OLED's tree for SEED before the Erista had a tree of its own.
OLED_ROOT = '4c008dbf82aace03a8457e6e5eaceee93dc61d30398db5e1801a55c3cca69c2a'
OLED_BARE = 'ef73246b54ab09c8649a21c331b01cdc4c9f24e70c72f9744bca07dbadb4f0f0'


def sha(data):
    return hashlib.sha256(data).hexdigest()


class OledAdtTest(unittest.TestCase):
    def test_the_oled_tree_did_not_change(self):
        self.assertEqual(sha(adt.generate_adt(SEED, ROOT)), OLED_ROOT)
        self.assertEqual(sha(adt.generate_adt(SEED, BARE)), OLED_BARE)

    def test_the_oled_tree_names_the_oled_and_nothing_of_the_erista(self):
        tree = adt.generate_adt(SEED, ROOT)
        for text in (b'Nintendo Switch (OLED model)\0', b'nintendo,aula\0', b'nvidia,tegra210b01\0'):
            self.assertIn(text, tree)
        for text in (b'2017', b'icosa', b'odin'):
            self.assertNotIn(text, tree)

    def test_the_oled_generator_takes_no_board(self):
        with self.assertRaises(TypeError):
            adt.generate_adt(SEED, BARE, board='erista')  # type: ignore[call-arg]


if __name__ == '__main__':
    unittest.main()
