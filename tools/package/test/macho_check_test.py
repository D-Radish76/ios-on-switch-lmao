"""macho_check on synthetic Mach-O files: what it refuses, what it accepts as normal, and the mapping to the boot window.

The files are built here. Where a figure is taken from the Mac's report of the real kernel (2026-11-27, `early_trace=true`:
__TEXT, __TEXT_EXEC, __PRELINK_TEXT and __start) it is marked; every other number is invented for the test.
"""

import struct
import tempfile
import unittest
from pathlib import Path

import first_xnu_check as check
import macho_check as macho

K = 0x4000
# Reported by the Mac for the real kernel.
TEXT = ('__TEXT', 0xFFFFFFF007004000, 0x1DC000, 1, 1)
TEXT_EXEC = ('__TEXT_EXEC', 0xFFFFFFF007400000, 0xA78000, 5, 5)
PRELINK_TEXT = ('__PRELINK_TEXT', 0xFFFFFFF00427C000, 0x6A4000, 1, 1)
START = 0xFFFFFFF0075CC4C8
START_PHYSICAL = 0xB53CC4C8
BASE = 0xFFFFFFF00427C000


def segment(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot):
    command = struct.pack(
        '<II16sQQQQIIII',
        macho.LC_SEGMENT_64,
        72,
        name.encode(),
        vmaddr,
        vmsize,
        fileoff,
        filesize,
        maxprot,
        initprot,
        0,
        0,
    )
    return command


def thread(entry):
    state = bytearray(68 * 4)
    struct.pack_into('<Q', state, 256, entry)
    return struct.pack('<IIII', macho.LC_UNIXTHREAD, 288, 6, 68) + bytes(state)


def kernel(segments, entry=START, symbol=None, size=0x504000, extra=()):
    commands = [*segments, thread(entry), *extra]
    if symbol is not None:
        strings = b'\0\0' + b'__start\0'
        table = struct.pack('<IBBHQ', 2, 0xF, 1, 0, symbol)
        offset = 0x500100
        commands.append(struct.pack('<IIIIII', macho.LC_SYMTAB, 24, offset, 1, offset + 16, len(strings)))
    body = b''.join(commands)
    header = struct.pack('<8I', macho.MAGIC, macho.CPU_ARM64, 0, macho.KIND_EXECUTE, len(commands), len(body), 0, 0)
    image = bytearray(header + body)
    image.extend(b'\0' * (size - len(image)))
    if symbol is not None:
        image[offset : offset + len(table)] = table
        image[offset + 16 : offset + 16 + len(strings)] = strings
    return bytes(image)


def seg(spec, fileoff=0, filesize=None):
    name, vmaddr, vmsize, maxprot, initprot = spec
    return segment(name, vmaddr, vmsize, fileoff, vmsize if filesize is None else filesize, maxprot, initprot)


def good():
    return [
        seg(PRELINK_TEXT, 0x10000, 0x20000),
        seg(('__TEXT', TEXT[1], 0x8000, 1, 1), 0x30000, 0x8000),
        seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x400000, 5, 5), 0x40000, 0x400000),
        segment('__LAST', 0xFFFFFFF008000000, 0, 0, 0, 5, 5),
        segment('__LINKEDIT', 0xFFFFFFF009000000, 0x4000, 0x500000, 0x4000, 1, 1),
    ]


def run(data, *flags):
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch, 'kernel.macho')
        path.write_bytes(data)
        return macho.check(path, *flags) if flags else macho.check(path, None, macho.DEFAULT_PHYS_BASE, None)


def errors(data, **options):
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch, 'kernel.macho')
        path.write_bytes(data)
        return macho.check(
            path, options.get('base'), options.get('phys_base', macho.DEFAULT_PHYS_BASE), options.get('entry_phys')
        )[1]


class Accepts(unittest.TestCase):
    def test_a_normal_kernel_has_no_errors(self):
        self.assertEqual([], errors(kernel(good(), symbol=START)))

    def test_empty_segments_zerofill_and_an_unaligned_file_offset_are_normal(self):
        segments = good()
        segments.insert(2, seg(('__DATA', 0xFFFFFFF007E00000, 0x8000, 3, 3), 0x440001, 0x2000))
        image, found = run(kernel(segments, symbol=START))
        self.assertEqual([], found)
        text = '\n'.join(macho.notes(image))
        self.assertIn('__LAST: empty', text)
        self.assertIn('__DATA: zerofill', text)
        self.assertIn('file offset 0x440001', text)

    def test_the_reported_prelink_segment_with_its_real_file_range_is_accepted(self):
        # fileoff 25337856 (0x182a000) and filesize 6963200 are the Mac's report; the rest is invented.
        segments = [
            seg(PRELINK_TEXT, 25337856),
            seg(('__TEXT', TEXT[1], 0x8000, 1, 1), 0x30000, 0x8000),
            seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x400000, 5, 5), 0x40000, 0x400000),
            segment('__LINKEDIT', 0xFFFFFFF009000000, 0x4000, 0x1F00000, 0x4000, 1, 1),
        ]
        image, found = run(kernel(segments, symbol=START, size=0x1F04000))
        self.assertEqual([], found)
        self.assertIn('file offset 0x182a000', '\n'.join(macho.notes(image)))

    def test_no_symbol_table_is_a_note_not_an_error(self):
        image, found = run(kernel(good()))
        self.assertEqual([], found)
        self.assertIn('no __start symbol', '\n'.join(macho.notes(image)))

    def test_sizes_may_differ_between_two_kernels_and_are_only_listed(self):
        smaller = good()
        smaller[2] = seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x3FC000, 5, 5), 0x40000, 0x3FC000)
        first, second = run(kernel(good(), symbol=START))[0], run(kernel(smaller, symbol=START))[0]
        lines = macho.differences(first, second)
        self.assertEqual(1, len(lines))
        self.assertIn('__TEXT_EXEC', lines[0])
        self.assertEqual([], macho.differences(first, first))


class Refuses(unittest.TestCase):
    def one(self, mutate, expect, **options):
        segments = good()
        mutate(segments)
        found = errors(kernel(segments, symbol=START), **options)
        self.assertTrue(any(expect in line for line in found), (expect, found))

    def test_writable_and_executable(self):
        self.one(
            lambda s: s.__setitem__(2, seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x400000, 7, 5), 0x40000)),
            'writable and executable',
        )
        self.one(
            lambda s: s.__setitem__(2, seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x400000, 7, 7), 0x40000)),
            'writable and executable',
        )

    def test_initprot_beyond_maxprot(self):
        self.one(lambda s: s.__setitem__(1, seg(('__TEXT', TEXT[1], 0x8000, 1, 3), 0x30000)), 'not within maxprot')

    def test_overlapping_virtual_ranges(self):
        self.one(lambda s: s.__setitem__(1, seg(('__TEXT', PRELINK_TEXT[1] + K, 0x8000, 1, 1), 0x30000)), 'virtual: ')

    def test_overlapping_file_ranges(self):
        self.one(lambda s: s.__setitem__(1, seg(('__TEXT', TEXT[1], 0x8000, 1, 1), 0x10000)), 'file: ')

    def test_a_segment_outside_the_file(self):
        self.one(
            lambda s: s.__setitem__(2, seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x400000, 5, 5), 0x1F0000)), 'past the file'
        )

    def test_filesize_above_vmsize(self):
        self.one(lambda s: s.__setitem__(1, seg(('__TEXT', TEXT[1], 0x8000, 1, 1), 0x30000, 0x9000)), 'exceeds vmsize')

    def test_an_unaligned_virtual_range(self):
        self.one(lambda s: s.__setitem__(1, seg(('__TEXT', TEXT[1] + 0x1000, 0x8000, 1, 1), 0x30000)), 'multiple of')

    def test_linkedit_not_last_or_not_alone(self):
        self.one(lambda s: s.reverse(), '__LINKEDIT is not the last')
        self.one(
            lambda s: s.append(segment('__LINKEDIT', 0xFFFFFFF00A000000, 0x4000, 0x502000, 0x2000, 1, 1)),
            'exactly one __LINKEDIT',
        )

    def test_two_segments_with_one_name(self):
        self.one(lambda s: s.insert(0, s[1]), 'share a name')

    def test_the_entry(self):
        for entry, expect in (
            (TEXT[1] + 0x100, 'not in an executable'),
            (0xFFFFFFF009000000, 'not in an executable'),
            (START + 2, 'not 4-byte aligned'),
        ):
            found = errors(kernel(good(), entry=entry))
            self.assertTrue(any(expect in line for line in found), (hex(entry), found))

    def test_an_entry_in_the_zerofill_part_of_an_executable_segment(self):
        segments = good()
        segments[2] = seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x400000, 5, 5), 0x40000, 0x1000)
        found = errors(kernel(segments, entry=TEXT_EXEC[1] + 0x2000))
        self.assertTrue(any('file-backed' in line for line in found), found)

    def test_an_entry_that_differs_from_start(self):
        found = errors(kernel(good(), entry=START, symbol=START + 4))
        self.assertTrue(any('differs from __start' in line for line in found), found)

    def test_not_a_kernel(self):
        for data in (b'', b'\0' * 64, kernel(good())[:40]):
            with self.assertRaises(macho.MachoError):
                macho.parse(data)

    def test_two_threads_or_none(self):
        body = kernel(good(), extra=[thread(START)])
        with self.assertRaises(macho.MachoError):
            macho.parse(body)


class Window(unittest.TestCase):
    def window_kernel(self):
        segments = [
            seg(PRELINK_TEXT, 0x10000, 0x20000),
            seg(TEXT, 0x30000, 0x8000),
            seg(TEXT_EXEC, 0x40000, 0x400000),
            segment('__LINKEDIT', 0xFFFFFFF009000000, 0x4000, 0x500000, 0x4000, 1, 1),
        ]
        return kernel(segments, symbol=START, size=0x504000)

    def test_the_reported_entry_maps_to_the_reported_physical_entry(self):
        self.assertEqual([], errors(self.window_kernel(), base=BASE, entry_phys=START_PHYSICAL))
        self.assertEqual(START_PHYSICAL, macho.DEFAULT_PHYS_BASE + START - BASE)

    def test_a_wrong_base_entry_or_physical_base(self):
        data = self.window_kernel()
        self.assertTrue(any('not 0xfffffff004004000' in line for line in errors(data, base=0xFFFFFFF004004000)))
        self.assertTrue(any('maps to' in line for line in errors(data, entry_phys=START_PHYSICAL + 4)))
        self.assertTrue(any('modulo 2 MiB' in line for line in errors(data, phys_base=0xB2000000)))

    def test_a_kernel_the_layout_tool_refuses_is_refused(self):
        segments = good()
        segments[2] = seg(('__TEXT_EXEC', TEXT_EXEC[1], 0x400000, 5, 5), 0x40000, 0x100000)
        found = errors(kernel(segments, entry=TEXT_EXEC[1] + 0x200000))
        self.assertTrue(found)


class CommandLine(unittest.TestCase):
    def main(self, *arguments, data):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch, 'kernel.macho')
            path.write_bytes(data)
            return check.main(['macho', str(path), *arguments])

    def test_exit_codes(self):
        self.assertEqual(0, self.main(data=kernel(good(), symbol=START)))
        self.assertEqual(1, self.main(data=kernel(good(), entry=START + 2)))
        self.assertEqual(1, self.main(data=b'not a kernel'))

    def test_the_other_modes_are_as_before(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch, 'kernel')
            path.write_bytes(b'\0' * 64)
            self.assertEqual(1, check.main(['kernel', str(path)]))


if __name__ == '__main__':
    unittest.main()
