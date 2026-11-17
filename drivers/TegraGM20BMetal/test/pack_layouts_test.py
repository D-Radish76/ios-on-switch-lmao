import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'raw', 'pack'))
import packfile


class Layouts(unittest.TestCase):
    def record(self):
        return {
            'vs': 0,
            'fs': 1,
            'set_size': 256,
            'entries': [
                {'off': 0, 'kind': 'ubo'},
                {'off': 80, 'kind': 'fetch'},
                {'off': 84, 'kind': 'texture', 'smp': 1},
            ],
            'variants': [
                {'stage': 0, 'assume': 1, 'program': 2},
                {'stage': 1, 'assume': 1, 'program': 3},
                {'stage': 1, 'assume': 2, 'program': 4},
            ],
        }

    def report(self, records: list[dict], facts: int = 0):
        programs = [{'id': str(i)} for i in range(10)]
        pack = packfile.Pack(records, programs, [], {}, [], bytearray(), [], [{'whole': 3, 'fs': 5}])
        with tempfile.TemporaryDirectory() as directory, patch.object(packfile, 'chip_facts', return_value=facts):
            path = Path(directory) / 'layouts.txt'
            packfile.write_layouts(str(path), pack)
            return {tuple(row.split()[:2]): row.split()[2:] for row in path.read_text().splitlines()}

    def test_reachable_variant_and_split_pairs_keep_the_fetch_texture_only(self):
        rows = self.report([self.record()])
        self.assertEqual(set(rows), {('0', '1'), ('2', '3'), ('2', '5'), ('0', '4')})
        self.assertTrue(all(layout == ['100', '0:u', '80:t', '84:h'] for layout in rows.values()))

    def test_chip_facts_and_selector_assumptions_exclude_unreachable_pairs(self):
        record = self.record()
        record['variants'].insert(0, {'stage': 0, 'assume': 1 << 26, 'program': 6})
        record['selectors'] = {'assume': 1, 'configs': [(1, 7), (2, 8)]}
        rows = self.report([record], facts=1)
        self.assertEqual(
            set(rows), {('0', '1'), ('6', '3'), ('6', '5'), ('2', '3'), ('2', '5'), ('2', '7'), ('2', '8')}
        )

    def test_conflicting_pairs_stay_unknown_and_unique_pairs_keep_their_layout(self):
        first, second = self.record(), self.record()
        second['entries'] = [{'off': 80, 'kind': 'texture', 'smp': 1}]
        unique = self.record() | {'vs': 6, 'fs': 9, 'variants': []}
        rows = self.report([first, second, first, unique])
        self.assertEqual(rows, {('6', '9'): ['100', '0:u', '80:t', '84:h']})

    def test_vertex_only_states_have_no_fragment_layout(self):
        record = self.record() | {'fs': packfile.NO_PROGRAM, 'variants': []}
        self.assertEqual(self.report([record]), {})


if __name__ == '__main__':
    unittest.main()
