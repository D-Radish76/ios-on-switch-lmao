"""hekate_unpack.py on synthetic streams."""

import struct
import unittest

from hekate_unpack import (
    LOADER_BASE,
    PAYLOAD_01_LITERAL,
    PAYLOAD_SIZE_LITERALS,
    PAYLOAD_START_LITERAL,
    LzError,
    locate,
    lz_uncompress,
    unpack,
)

MARKER = 0x90


def image(first: bytes, second: bytes, start_00: int = 0x784, gap: int = 3) -> bytes:
    start_01 = start_00 + len(first) + gap
    data = bytearray(start_01 + len(second) + 8)
    data[start_00 : start_00 + len(first)] = first
    data[start_01 : start_01 + len(second)] = second
    for offset, value in (
        (PAYLOAD_SIZE_LITERALS[0], len(first)),
        (PAYLOAD_SIZE_LITERALS[1], len(second)),
        (PAYLOAD_START_LITERAL, LOADER_BASE + start_00),
        (PAYLOAD_01_LITERAL, LOADER_BASE + start_01),
    ):
        struct.pack_into('<I', data, offset, value)
    return bytes(data)


class Stream(unittest.TestCase):
    def test_plain_bytes_are_copied(self) -> None:
        self.assertEqual(b'abc', lz_uncompress(bytes([MARKER]) + b'abc'))

    def test_the_marker_followed_by_zero_is_a_literal_marker(self) -> None:
        self.assertEqual(bytes([MARKER]), lz_uncompress(bytes([MARKER, MARKER, 0])))

    def test_a_back_reference_copies_from_the_output(self) -> None:
        self.assertEqual(b'abcabc', lz_uncompress(bytes([MARKER]) + b'abc' + bytes([MARKER, 3, 3])))

    def test_an_overlapping_reference_repeats_the_last_byte(self) -> None:
        self.assertEqual(b'aaaaa', lz_uncompress(bytes([MARKER]) + b'a' + bytes([MARKER, 4, 1])))

    def test_a_multi_byte_length_is_read_seven_bits_at_a_time(self) -> None:
        out = lz_uncompress(bytes([MARKER]) + b'a' + bytes([MARKER, 0x81, 0x00, 1]))
        self.assertEqual(b'a' * 129, out)

    def test_an_offset_before_the_start_is_rejected(self) -> None:
        with self.assertRaises(LzError):
            lz_uncompress(bytes([MARKER]) + b'ab' + bytes([MARKER, 2, 3]))

    def test_a_zero_offset_is_rejected(self) -> None:
        with self.assertRaises(LzError):
            lz_uncompress(bytes([MARKER]) + b'a' + bytes([MARKER, 2, 0]))

    def test_a_truncated_reference_is_rejected(self) -> None:
        with self.assertRaises(LzError):
            lz_uncompress(bytes([MARKER]) + b'a' + bytes([MARKER, 2]))
        with self.assertRaises(LzError):
            lz_uncompress(bytes([MARKER]) + b'a' + bytes([MARKER]))
        with self.assertRaises(LzError):
            lz_uncompress(bytes([MARKER]) + b'a' + bytes([MARKER, 0x81]))

    def test_output_over_the_limit_is_rejected(self) -> None:
        with self.assertRaises(LzError):
            lz_uncompress(bytes([MARKER]) + b'a' + bytes([MARKER, 50, 1]), max_output=10)


class Image(unittest.TestCase):
    def test_two_streams_are_joined_in_order(self) -> None:
        first = bytes([MARKER]) + b'abc'
        second = bytes([MARKER]) + b'de'
        out, (s0, n0, s1, n1, first_len) = unpack(image(first, second, gap=0))
        self.assertEqual(b'abcde', out)
        self.assertEqual((0x784, 4, 0x784 + 4, 3, 3), (s0, n0, s1, n1, first_len))

    def test_a_range_past_the_image_is_rejected(self) -> None:
        data = bytearray(image(bytes([MARKER]) + b'a', bytes([MARKER]) + b'b'))
        struct.pack_into('<I', data, PAYLOAD_SIZE_LITERALS[1], 0x100000)
        with self.assertRaises(LzError):
            locate(bytes(data))

    def test_overlapping_streams_are_rejected(self) -> None:
        data = bytearray(image(bytes([MARKER]) + b'abcd', bytes([MARKER]) + b'b', gap=0))
        struct.pack_into('<I', data, PAYLOAD_01_LITERAL, LOADER_BASE + 0x784 + 2)
        with self.assertRaises(LzError):
            locate(bytes(data))

    def test_a_start_that_is_not_word_aligned_is_rejected(self) -> None:
        data = image(bytes([MARKER]) + b'abcd', bytes([MARKER]) + b'b', gap=3)
        with self.assertRaises(LzError):
            locate(image(bytes([MARKER]) + b'abcd', bytes([MARKER]) + b'b', start_00=0x785, gap=3))
        self.assertEqual(b'abcdb', unpack(data)[0])


if __name__ == '__main__':
    unittest.main()
