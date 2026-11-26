"""The seed scanner: what it finds, what it refuses to skip and what it says it cannot see. Synthetic seeds only."""

import os
import tempfile
import unittest
import zlib
from pathlib import Path

import seed_scan

SEED = bytes((i * 7 + 3) % 256 for i in range(256))


def fresh():
    return tempfile.TemporaryDirectory()


class Finds(unittest.TestCase):
    def test_raw_and_hex_copies_in_nested_directories_are_leaks(self):
        with fresh() as scratch:
            root = Path(scratch)
            (root / 'a' / 'b').mkdir(parents=True)
            (root / 'a' / 'raw').write_bytes(b'\0' * 5 + SEED)
            (root / 'a' / 'b' / 'hex').write_text('log ' + SEED[96:160].hex())
            (root / 'a' / 'b' / 'upper').write_text(SEED[96:160].hex().upper())
            (root / 'clean').write_bytes(b'nothing')
            report = seed_scan.scan(SEED, [root])
            self.assertEqual({p.name for p in report.leaks}, {'raw', 'hex', 'upper'})
            self.assertEqual(report.scanned, 4)
            self.assertFalse(report.clean)

    def test_a_copy_across_a_read_boundary_is_found(self):
        with fresh() as scratch:
            path = Path(scratch) / 'big'
            path.write_bytes(b'\1' * (seed_scan.CHUNK - 30) + SEED + b'\1' * 10)
            self.assertEqual(seed_scan.scan(SEED, [path]).leaks, [path])

    def test_the_pinned_seed_prefix_is_not_the_needle(self):
        with fresh() as scratch:
            path = Path(scratch) / 'f'
            path.write_bytes(SEED[:64])
            self.assertTrue(seed_scan.scan(SEED, [path]).clean)

    def test_expected_places_count_hits_and_are_not_leaks(self):
        with fresh() as scratch:
            outputs, other = Path(scratch) / 'out', Path(scratch) / 'other'
            outputs.write_bytes(SEED)
            other.write_bytes(b'x')
            report = seed_scan.scan(SEED, [other], [outputs])
            self.assertTrue(report.clean)
            self.assertEqual(report.expected_hits, [outputs])
            self.assertEqual((report.scanned, report.expected_scanned), (1, 1))


class Refuses(unittest.TestCase):
    def test_a_missing_place_is_an_error_for_both_kinds(self):
        with fresh() as scratch:
            with self.assertRaises(seed_scan.ScanError):
                seed_scan.scan(SEED, [Path(scratch) / 'nope'])
            with self.assertRaises(seed_scan.ScanError):
                seed_scan.scan(SEED, [], [Path(scratch) / 'nope'])

    @unittest.skipIf(os.getuid() == 0, 'root reads everything')
    def test_an_unreadable_file_is_reported_and_the_scan_is_not_clean(self):
        with fresh() as scratch:
            secret = Path(scratch) / 'locked'
            secret.write_bytes(SEED)
            secret.chmod(0)
            report = seed_scan.scan(SEED, [Path(scratch)])
            self.assertEqual(report.unreadable, [secret])
            self.assertFalse(report.clean)

    def test_an_unreadable_directory_is_reported(self):
        if os.getuid() == 0:
            self.skipTest('root reads everything')
        with fresh() as scratch:
            locked = Path(scratch) / 'locked'
            locked.mkdir()
            (locked / 'f').write_bytes(SEED)
            locked.chmod(0)
            try:
                report = seed_scan.scan(SEED, [Path(scratch)])
            finally:
                locked.chmod(0o700)
            self.assertFalse(report.clean)


class Links(unittest.TestCase):
    def test_links_are_not_followed_and_a_cycle_ends(self):
        with fresh() as scratch:
            root = Path(scratch)
            (root / 'real').mkdir()
            (root / 'real' / 'f').write_bytes(b'x')
            (root / 'real' / 'loop').symlink_to(root)
            (root / 'to_file').symlink_to(root / 'real' / 'f')
            outside = Path(scratch + '-outside')
            outside.write_bytes(SEED)
            try:
                (root / 'to_outside').symlink_to(outside)
                report = seed_scan.scan(SEED, [root])
            finally:
                outside.unlink()
            self.assertTrue(report.clean)
            self.assertEqual({p.name for p in report.links}, {'loop', 'to_file', 'to_outside'})
            self.assertEqual(report.scanned, 1)

    def test_a_root_that_is_a_link_to_a_directory_is_scanned_once(self):
        with fresh() as scratch:
            real, link = Path(scratch) / 'real', Path(scratch) / 'link'
            real.mkdir()
            (real / 'f').write_bytes(SEED)
            link.symlink_to(real)
            self.assertEqual(seed_scan.scan(SEED, [link]).leaks, [real.resolve() / 'f'])

    def test_special_files_are_reported_and_not_opened(self):
        with fresh() as scratch:
            fifo = Path(scratch) / 'pipe'
            os.mkfifo(fifo)
            report = seed_scan.scan(SEED, [Path(scratch)])
            self.assertEqual(report.special, [fifo])


class SaysWhatItCannotSee(unittest.TestCase):
    def test_a_compressed_copy_is_not_found_and_the_summary_says_so(self):
        with fresh() as scratch:
            path = Path(scratch) / 'blob.z'
            path.write_bytes(zlib.compress(b'header ' + SEED * 40))
            report = seed_scan.scan(SEED, [path])
            self.assertTrue(report.clean)
            text = '\n'.join(seed_scan.summary(report))
            for word in ('compressed', 'base64', 'nobody listed'):
                self.assertIn(word, text)

    def test_the_summary_never_holds_the_seed(self):
        with fresh() as scratch:
            path = Path(scratch) / 'f'
            path.write_bytes(SEED)
            text = '\n'.join(seed_scan.summary(seed_scan.scan(SEED, [path])))
            self.assertNotIn(SEED[96:160].hex(), text)
            self.assertIn('scanned 1 files in 1 places', text)


if __name__ == '__main__':
    unittest.main()
