"""The pinned bl33.bin's reserve-map adders: three callers, and the shrink function adds its entry unconditionally."""

import unittest
from pathlib import Path

from bl33_reserve_audit import analyse

BL33 = Path('bazel/pinned/firmware/bl33.bin').read_bytes()
FOUND = analyse(BL33)


class ReserveMapAdders(unittest.TestCase):
    def test_exactly_three_places_add_an_entry(self) -> None:
        self.assertEqual(len(FOUND['add_callers']), 3)

    def test_those_are_the_rsvmem_command_the_initrd_fixup_and_the_shrink_function(self) -> None:
        self.assertIn(FOUND['shrink_add_call'], FOUND['add_callers'])
        self.assertEqual(sum(1 for c in FOUND['add_callers'] if c < FOUND['shrink_start']), 2)

    def test_the_shrink_function_has_a_loop_exit_before_its_add(self) -> None:
        self.assertGreater(FOUND['shrink_join'], FOUND['shrink_start'])
        self.assertLess(FOUND['shrink_join'], FOUND['shrink_add_call'])

    def test_no_condition_stands_between_the_loop_exit_and_the_add(self) -> None:
        self.assertEqual(FOUND['conditional_between_join_and_add'], [])

    def test_it_is_called_from_two_places_the_resize_command_and_the_boot_path(self) -> None:
        self.assertEqual(len(FOUND['shrink_callers']), 2)


if __name__ == '__main__':
    unittest.main()
