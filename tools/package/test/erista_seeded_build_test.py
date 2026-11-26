"""The seeded build's wrapper and the cache promises behind it: what it refuses, what it passes to bazel, what its scan finds."""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import erista_seed
import erista_seeded_build as wrapper

ROOT = Path('.')
DEFS = (ROOT / 'boot/defs.bzl').read_text()
BUILD = (ROOT / 'boot/BUILD.bazel').read_text()


def fake_run(tracked=False, ignored=True):
    def run(command, **_):
        if command[:2] == ['git', 'ls-files']:
            return subprocess.CompletedProcess(command, 0 if tracked else 1, b'', b'')
        return subprocess.CompletedProcess(command, 0 if ignored else 1)

    return run


class SeededBuildTest(unittest.TestCase):
    def test_git_must_ignore_the_seed_and_not_track_it(self):
        wrapper.git_clean(Path('local/erista-seed.hex'), fake_run())
        with self.assertRaises(wrapper.BuildError):
            wrapper.git_clean(Path('local/erista-seed.hex'), fake_run(tracked=True))
        with self.assertRaises(wrapper.BuildError):
            wrapper.git_clean(Path('local/erista-seed.hex'), fake_run(ignored=False))

    def test_the_bazel_command_has_every_cache_flag_and_no_seed(self):
        command = wrapper.bazel_command()
        for flag in erista_seed.CACHE_FLAGS:
            self.assertIn(flag, command)
        self.assertEqual(command[-1], '//boot:erista_first_xnu_seeded')
        self.assertIn('--//kernel:early_trace=true', command)
        self.assertNotRegex(' '.join(command), r'[0-9a-f]{64}')

    def test_flags_that_would_log_build_details_are_refused(self):
        for flag in ('-s', '--subcommands', '--verbose_failures', '--build_event_json_file=x', '--profile=x'):
            with self.subTest(flag=flag), self.assertRaises(wrapper.BuildError):
                wrapper.bazel_command([flag])

    def test_scan_finds_hex_and_raw_copies_and_not_clean_files(self):
        seed = bytes(range(256))[::-1]
        with tempfile.TemporaryDirectory() as scratch:
            clean_file, hex_file, raw_file = (Path(scratch) / n for n in ('a', 'b', 'c'))
            clean_file.write_bytes(erista_seed.PINNED_SEED)
            hex_file.write_text('log ' + seed.hex())
            raw_file.write_bytes(b'\0' * 7 + seed)
            self.assertEqual(wrapper.scan(seed, [clean_file, hex_file, raw_file]), [hex_file, raw_file])

    def test_every_action_of_the_seeded_package_is_private(self):
        block = BUILD[BUILD.index('name = "erista_first_xnu_seeded"') :].split('\n)\n')[0]
        self.assertIn('private_seed = True', block)
        self.assertEqual(BUILD.count('private_seed = True'), 1)
        raw = len(re.findall(r'ctx\.actions\.run(?:_shell)?\(', DEFS))
        guarded = len(re.findall(r'execution_requirements = _requirements\(ctx', DEFS))
        self.assertEqual(raw, guarded, 'an action of boot_package would be cacheable with a seed in it')
        self.assertIn('"no-cache": "1", "no-remote": "1"', DEFS)

    def test_the_seed_never_reaches_an_action_argument(self):
        target = BUILD[BUILD.index('name = "erista_adt_seeded"') :].split('\n)\n')[0]
        self.assertIn('$(execpath //local:erista-seed.hex)', target)
        self.assertNotRegex(target, r'[0-9a-f]{64}')


if __name__ == '__main__':
    unittest.main()
