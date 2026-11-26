"""The per-install Erista seed: generation, refusal when unset, and that the seeded target keeps it out of caches."""

import os
import re
import stat
import tempfile
import unittest
from pathlib import Path

import erista_seed

ROOT = Path('.')
BUILD = (ROOT / 'boot/BUILD.bazel').read_text()
LOCAL_IGNORE = (ROOT / 'local/.gitignore').read_text()
SEEDED_ADT = (ROOT / 'tools/package/erista_adt_seeded.py').read_text()


class EristaSeedTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.path = Path(self.scratch.name) / 'seed.hex'

    def tearDown(self):
        self.scratch.cleanup()

    def test_pinned_seed_is_the_one_in_the_build_file(self):
        self.assertEqual(erista_seed.PINNED_SEED.hex(), re.search(r'_ADT_SEED = "(\w+)"', BUILD).group(1))

    def test_generate_writes_a_private_random_seed(self):
        erista_seed.generate(self.path)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        seed = erista_seed.load(self.path)
        self.assertEqual(len(seed), 256)
        other = Path(self.scratch.name) / 'other.hex'
        erista_seed.generate(other)
        self.assertNotEqual(seed, erista_seed.load(other))

    def test_generate_does_not_replace_without_force(self):
        erista_seed.generate(self.path)
        first = self.path.read_text()
        with self.assertRaises(erista_seed.SeedError):
            erista_seed.generate(self.path)
        self.assertEqual(self.path.read_text(), first)
        erista_seed.generate(self.path, force=True)
        self.assertNotEqual(self.path.read_text(), first)

    def test_missing_seed_is_refused(self):
        with self.assertRaises(erista_seed.SeedError):
            erista_seed.load(self.path)
        self.assertEqual(erista_seed.main(['check', str(self.path)]), 1)

    def test_bad_seeds_are_refused(self):
        for text in (
            erista_seed.PINNED_SEED.hex(),
            erista_seed.PINNED_SEED[:64].hex() + os.urandom(192).hex(),
            '00' * 256,
            'ab' * 255,
            os.urandom(256).hex()[:-1],
            'zz' * 256,
            bytes(range(50)).hex() * 5 + "00" * 6,
        ):
            self.path.write_text(text)
            self.path.chmod(0o600)
            with self.subTest(text=text[:16]), self.assertRaises(erista_seed.SeedError):
                erista_seed.load(self.path)

    def test_a_world_readable_seed_is_refused(self):
        erista_seed.generate(self.path)
        self.path.chmod(0o644)
        with self.assertRaises(erista_seed.SeedError):
            erista_seed.load(self.path)

    def test_the_seed_is_never_printed(self):
        import contextlib
        import io

        erista_seed.generate(self.path)
        seed = self.path.read_text().strip()
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            erista_seed.main(['check', str(self.path)])
            erista_seed.main(['command', str(self.path)])
        self.assertNotIn(seed[:32], captured.getvalue())

    def test_the_seeded_target_stays_out_of_caches_and_git(self):
        target = BUILD[BUILD.index('name = "erista_adt_seeded"') :][:700]
        self.assertIn('"no-cache"', target)
        self.assertIn('"no-remote"', target)
        self.assertIn('erista-seed.hex', LOCAL_IGNORE)
        command = erista_seed.command()
        for flag in ('--noremote_upload_local_results', '--noremote_accept_cached', '--disk_cache='):
            self.assertIn(flag, command)
        self.assertIn('erista_first_xnu_seeded', command)
        self.assertIn('erista_seed.load', SEEDED_ADT)

    def test_the_pinned_targets_are_untouched(self):
        for name in ('erista_adt', 'erista_package', 'erista_first_xnu'):
            block = BUILD[BUILD.index(f'name = "{name}"') :][:300]
            self.assertNotIn('seeded', block.split(')')[0])


if __name__ == '__main__':
    unittest.main()
