"""fdt_ranges.py over the pinned DTBs and over every way a tree can be malformed.

Nothing is returned for a tree the reader does not fully understand.
"""

import struct
import unittest
from pathlib import Path

from fdt_ranges import reserved_ranges
from fdt_tree import BEGIN, END, END_NODE, NOP, PROP, STRINGS, node, prop, reg, reserved, tree, word

ROOT = Path('.')
PINNED = {name: (ROOT / f'bazel/pinned/firmware/{name}.dtb').read_bytes() for name in ('erista-sku0', 'oled-sku3')}
KNOWN = [
    ('memreserve', 0x80000000, 0x80020000),
    ('/reserved-memory/ramoops_carveout', 0xB0000000, 0xB0200000),
]
HEADER_FIELDS = (
    'magic',
    'total',
    'off_struct',
    'off_strings',
    'off_rsv',
    'version',
    'last',
    'cpu',
    'size_strings',
    'size_struct',
)


def patched(dtb: bytes, **fields: int) -> bytes:
    values = dict(zip(HEADER_FIELDS, struct.unpack('>10I', dtb[:40]), strict=True))
    values.update(fields)
    return struct.pack('>10I', *values.values()) + dtb[40:]


def root(*children: bytes) -> bytes:
    return node('', prop('#address-cells', word(2)) + prop('#size-cells', word(2)) + b''.join(children))


class Pinned(unittest.TestCase):
    def test_both_boards_reserve_only_what_is_known(self) -> None:
        for name, dtb in PINNED.items():
            self.assertEqual(reserved_ranges(dtb), KNOWN, name)

    def test_the_control_fdt_in_bl33_reserves_nothing_and_its_memory_is_a_placeholder(self) -> None:
        u_boot = (ROOT / 'bazel/pinned/firmware/bl33.bin').read_bytes()
        at = u_boot.find(b'\xd0\x0d\xfe\xed')
        self.assertGreater(at, 0)
        self.assertEqual(reserved_ranges(u_boot[at:]), [])
        self.assertIn(bytes.fromhex('00000000800000000000000180000000'), u_boot[at:])


class Readable(unittest.TestCase):
    def test_both_kinds_are_found_and_a_carveout_of_size_zero_is_skipped(self) -> None:
        got = reserved_ranges(tree([(0x1000, 0x100)], {'a': (0xB0000000, 0x1000), 'empty': (0, 0)}))
        self.assertEqual(got, [('memreserve', 0x1000, 0x1100), ('/reserved-memory/a', 0xB0000000, 0xB0001000)])

    def test_a_tree_without_reserved_memory_has_only_its_memreserve(self) -> None:
        self.assertEqual(
            reserved_ranges(tree([(0x2000, 0x10)], {}, structure=root())), [('memreserve', 0x2000, 0x2010)]
        )

    def test_one_cell_addresses_and_sizes_are_read(self) -> None:
        structure = root(reserved({'x': reg(0x9000, 0x40, (1, 1))}, (1, 1)))
        self.assertEqual(reserved_ranges(tree([], {}, structure=structure)), [('/reserved-memory/x', 0x9000, 0x9040)])

    def test_the_top_of_the_address_space_is_allowed_when_it_does_not_pass_it(self) -> None:
        got = reserved_ranges(tree([], {'x': (0xFFFFFFFFFFFFF000, 0x1000)}))
        self.assertEqual(got, [('/reserved-memory/x', 0xFFFFFFFFFFFFF000, 1 << 64)])

    def test_nops_between_tokens_are_ignored(self) -> None:
        structure = word(NOP) + root(reserved({'x': reg(0x9000, 0x40)}))
        self.assertEqual(reserved_ranges(tree([], {}, structure=structure)), [('/reserved-memory/x', 0x9000, 0x9040)])


class Refused(unittest.TestCase):
    def refuse(self, dtb: bytes, message: str = '') -> None:
        with self.assertRaises(ValueError) as caught:
            reserved_ranges(dtb)
        self.assertIn(message, str(caught.exception))

    def test_header_problems(self) -> None:
        good = tree([], {})
        self.refuse(good[:30], 'shorter')
        self.refuse(patched(good, magic=0), 'magic')
        self.refuse(patched(good, version=16, last=16), 'version')
        self.refuse(patched(good, last=18), 'version')
        self.refuse(patched(good, total=len(good) + 4), 'total size')
        self.refuse(patched(good, total=20), 'total size')

    def test_the_real_header_with_one_field_changed(self) -> None:
        for name, dtb in PINNED.items():
            header = struct.unpack('>10I', dtb[:40])
            fields = dict(zip(HEADER_FIELDS, header, strict=True))
            cases = {
                'version 16': {'version': 16, 'last': 16},
                'size_struct 0': {'size_struct': 0},
                'size_struct cut in half': {'size_struct': fields['size_struct'] // 2 // 4 * 4},
                'size_struct not a word multiple': {'size_struct': fields['size_struct'] - 2},
                'strings block empty': {'size_strings': 0},
                'strings block too small': {'size_strings': 40},
                'off_struct unaligned': {'off_struct': fields['off_struct'] + 1},
                'off_rsv unaligned': {'off_rsv': 44},
                'rsvmap past the end': {'off_rsv': fields['total'] - 8},
                'strings over the structure': {'off_strings': fields['off_struct'] + 8},
                'structure over the rsvmap': {'off_struct': 40},
                'strings past the end': {'off_strings': fields['total'] - 4},
                'structure past the end': {'off_struct': fields['total'] - 4},
            }
            for label, change in cases.items():
                with self.subTest(name=name, case=label), self.assertRaises(ValueError):
                    reserved_ranges(patched(dtb, **change))

    def test_the_end_of_the_structure_block(self) -> None:
        self.refuse(tree([], {}, end=word(NOP)), 'FDT_END')
        self.refuse(tree([], {}, end=word(END) + word(NOP)), 'bytes after')
        self.refuse(tree([], {}, end=word(END_NODE) + word(END)), 'no node open')
        self.refuse(tree([], {}, structure=root(reserved({}), b'')[:-4]), 'node still open')

    def test_node_hierarchy(self) -> None:
        self.refuse(tree([], {}, structure=root() + root()), 'second root')
        self.refuse(tree([], {}, structure=root(node('a'), node('a'))), 'twice')
        self.refuse(tree([], {}, structure=prop('reg') + root()), 'outside every node')
        self.refuse(tree([], {}, structure=word(BEGIN) + b'abcd', end=b'efgh'), 'not terminated')
        self.refuse(tree([], {}, structure=root(word(7))), 'unknown FDT token')

    def test_properties(self) -> None:
        self.refuse(tree([], {}, structure=root(prop('reg', b'\0' * 4, length=0x1000))), 'leaves its block')
        self.refuse(tree([], {}, structure=root(prop('reg', name_offset=len(STRINGS)))), 'leaves its block')
        self.refuse(tree([], {}, structure=root(prop('reg') + prop('reg'))), 'twice')
        self.refuse(tree([], {}, structure=word(BEGIN) + word(0) + word(PROP), end=word(NOP)), 'property header')

    def test_a_property_name_must_end_inside_the_strings_block(self) -> None:
        dtb = tree([], {}, structure=root(prop('reg', name_offset=len(STRINGS) - 1)))
        self.refuse(patched(dtb, size_strings=len(STRINGS) - 1), 'leaves its block')

    def test_ranges_past_64_bits(self) -> None:
        self.refuse(tree([], {'x': (0xFFFFFFFFFFFFF000, 0x2000)}), '2**64')
        self.refuse(tree([(0xFFFFFFFFFFFFFFF0, 0x100)], {}), '2**64')

    def test_cell_counts(self) -> None:
        for cells in ((3, 2), (2, 3), (0, 2), (2, 0)):
            structure = root(reserved({'x': reg(0x9000, 0x40)}, cells))
            with self.subTest(cells=cells):
                self.refuse(tree([], {}, structure=structure), 'only 1 or 2')
        no_cells = root(node('reserved-memory', prop('ranges')))
        self.refuse(tree([], {}, structure=no_cells), 'one word')
        short = root(node('reserved-memory', prop('#address-cells', b'\0\0') + prop('#size-cells', word(2))))
        self.refuse(tree([], {}, structure=short), 'one word')

    def test_reserved_memory_layout(self) -> None:
        carveout = {'x': reg(0x9000, 0x40)}
        self.refuse(tree([], {}, structure=root(reserved(carveout, ranges=False))), 'ranges')
        mapped = node(
            'reserved-memory',
            prop('#address-cells', word(2)) + prop('#size-cells', word(2)) + prop('ranges', word(0)),
        )
        self.refuse(tree([], {}, structure=root(mapped)), 'ranges')
        nested = node('outer', prop('reg', reg(0x9000, 0x40)) + node('inner', prop('reg', reg(0xA000, 0x40))))
        parent = node(
            'reserved-memory',
            prop('#address-cells', word(2)) + prop('#size-cells', word(2)) + prop('ranges') + nested,
        )
        self.refuse(tree([], {}, structure=root(parent)), 'nested')
        pool = node(
            'reserved-memory',
            prop('#address-cells', word(2)) + prop('#size-cells', word(2)) + prop('ranges') + node('pool'),
        )
        self.refuse(tree([], {}, structure=root(pool)), 'no reg')
        self.refuse(tree([], {}, structure=root(reserved({'x': b'\0' * 12}))), 'whole number')

    def test_the_failure_is_never_an_empty_answer(self) -> None:
        for name, dtb in PINNED.items():
            for cut in (45, 100, len(dtb) // 2, len(dtb) - 1):
                with self.subTest(name=name, cut=cut), self.assertRaises(ValueError):
                    reserved_ranges(dtb[:cut])


if __name__ == '__main__':
    unittest.main()
