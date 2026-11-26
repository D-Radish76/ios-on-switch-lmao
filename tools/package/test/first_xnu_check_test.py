"""first_xnu_check on synthetic kernels and loader images, and the wiring that makes the package demand both."""

import struct
import tempfile
import unittest
from pathlib import Path

import first_xnu_check as check

ROOT = Path('.')
DEFS = (ROOT / 'boot/defs.bzl').read_text()
BUILD = (ROOT / 'boot/BUILD.bazel').read_text()
PADDING = struct.pack('<I', 0xD503201F) * 8
PLAIN = struct.pack('<2I', 0xD280003C, 0xF900021C) + struct.pack('<I', 0xB4000000)


def uimage(size):
    header = struct.pack('>I8xI', check.UIMAGE_MAGIC, size).ljust(check.UIMAGE_HEADER, b'\0')
    return header + b'\0' * size


class FirstXnuCheckTest(unittest.TestCase):
    def kernel_check(self, body):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / 'kernel'
            path.write_bytes(body)
            return check.main(['kernel', str(path)])

    def test_trace_kernel_passes(self):
        self.assertEqual(self.kernel_check(PADDING + check.TRACE_SEQUENCE + PADDING), 0)

    def test_plain_kernel_is_refused(self):
        self.assertEqual(self.kernel_check(PADDING + PLAIN + PADDING), 1)

    def test_kernel_without_the_early_path_is_refused(self):
        self.assertEqual(self.kernel_check(PADDING), 1)

    def test_stamp_is_written_only_on_success(self):
        with tempfile.TemporaryDirectory() as scratch:
            kernel, stamp = Path(scratch) / 'kernel', Path(scratch) / 'stamp'
            kernel.write_bytes(PLAIN)
            self.assertEqual(check.main(['kernel', str(kernel), '--stamp', str(stamp)]), 1)
            self.assertFalse(stamp.exists())
            kernel.write_bytes(check.TRACE_SEQUENCE)
            self.assertEqual(check.main(['kernel', str(kernel), '--stamp', str(stamp)]), 0)
            self.assertTrue(stamp.exists())

    def test_margin(self):
        margin = check.loader_margin(uimage(0x1E000))
        self.assertEqual(margin['to_stamps_slot'], 0xF00)
        self.assertEqual(margin['to_sd_reference'], 0x1000)
        self.assertLess(check.loader_margin(uimage(0x1EF80))['to_stamps_slot'], 0)

    def test_margin_refuses_a_truncated_image(self):
        with self.assertRaises(ValueError):
            check.loader_margin(uimage(0x1000)[:-1])

    def test_package_rule_runs_the_check(self):
        self.assertIn('require_early_trace', DEFS)
        self.assertIn('first_xnu_check', DEFS)
        entry = BUILD[BUILD.index('name = "erista_first_xnu"') :][:400]
        self.assertIn('require_early_trace = True', entry)
        self.assertEqual(BUILD.count('require_early_trace = True'), 2)  # the plain and the seeded first-XNU targets


if __name__ == '__main__':
    unittest.main()
