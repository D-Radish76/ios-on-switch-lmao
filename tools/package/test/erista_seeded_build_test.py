"""The seeded build's wrapper and the cache promises behind it: what it refuses, what it passes to bazel, what its scan finds."""

import re
import subprocess
import tempfile
from unittest import mock
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


SPELLINGS = (
    '--//boot:erista_boot_run=true',
    '--//boot:erista_boot_run=True',
    '--//boot:erista_boot_run=1',
    '--//boot:erista_boot_run',
    '--@//boot:erista_boot_run=true',
    '--@@//boot:erista_boot_run=true',
    '--//BOOT:ERISTA_BOOT_RUN=true',
    '--//boot:erista_boot_run=false',
    '--flag_alias=go=//boot:erista_boot_run',
    '//boot:erista_boot_run',
)


def aquery_run(stdout='Mnemonic: BootScript\n', returncode=0):
    seen = []

    def run(command, **_):
        seen.append(command)
        return subprocess.CompletedProcess(command, returncode, stdout, '')

    run.seen = seen
    return run


class BootRowIsRefused(unittest.TestCase):
    def test_the_flag_in_any_spelling_or_repeated_is_refused(self):
        for spelling in SPELLINGS:
            with self.subTest(spelling=spelling), self.assertRaises(wrapper.BuildError):
                wrapper.bazel_command([spelling])
        with self.assertRaises(wrapper.BuildError):
            wrapper.bazel_command(['--//boot:erista_boot_run=false', '--//boot:erista_boot_run=true'])

    def test_options_that_bring_in_unseen_flags_are_refused(self):
        for option in ('--config=x', '--bazelrc=x', '--flagfile=x', '--flag_alias=a=//b:c', '--noworkspace_rc', '@args.txt'):
            with self.subTest(option=option), self.assertRaises(wrapper.BuildError):
                wrapper.bazel_command([option])

    def test_the_command_ends_with_the_flag_forced_off_and_the_target(self):
        command = wrapper.bazel_command(['--jobs=4'])
        self.assertEqual(command[-2:], ['--//boot:erista_boot_run=false', '//boot:erista_first_xnu_seeded'])
        self.assertEqual(1, sum(erista_flag in c for c in command for erista_flag in ('erista_boot_run',)))
        self.assertIn('--jobs=4', command)
        self.assertIn('--//install:prebuilt=local', command)

    def test_the_boot_0_seeded_build_keeps_working(self):
        command = wrapper.bazel_command()
        self.assertEqual(command[:2], ['bazel', 'build'])
        self.assertIn('--//kernel:early_trace=true', command)

    def test_an_rc_file_that_names_the_flag_is_refused_whatever_it_sets(self):
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / '.bazelrc').write_text('build --//boot:erista_boot_run=true\n')
            (Path(scratch) / 'user.bazelrc').write_text('build --config=ios17\n')
            with self.assertRaises(wrapper.BuildError):
                wrapper.rc_files_clean(wrapper.rc_files(Path(scratch), Path(scratch) / 'home'))
            home = Path(scratch) / 'home'
            home.mkdir()
            (home / '.bazelrc').write_text('# BUILD --//boot:ERISTA_BOOT_RUN=1\n')
            (Path(scratch) / '.bazelrc').write_text('build --jobs=2\n')
            with self.assertRaises(wrapper.BuildError):
                wrapper.rc_files_clean(wrapper.rc_files(Path(scratch), home))

    def test_clean_rc_files_pass(self):
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / '.bazelrc').write_text('build --jobs=2\n')
            wrapper.rc_files_clean(wrapper.rc_files(Path(scratch), Path(scratch) / 'none'))

    def test_the_analysis_check_refuses_a_derived_board_table(self):
        for mnemonic in ('BoardHeader', 'BoardAgree'):
            with self.subTest(mnemonic=mnemonic), self.assertRaises(wrapper.BuildError):
                wrapper.boot_row_is_off((), aquery_run(f'Mnemonic: BootScript\nMnemonic: {mnemonic}\n'))

    def test_the_analysis_check_fails_closed(self):
        with self.assertRaises(wrapper.BuildError):
            wrapper.boot_row_is_off((), aquery_run(returncode=1))
        with self.assertRaises(wrapper.BuildError):
            wrapper.boot_row_is_off((), aquery_run(stdout=''))

    def test_the_analysis_check_passes_a_boot_0_package_and_asks_about_the_same_command(self):
        run = aquery_run()
        wrapper.boot_row_is_off(['--jobs=4'], run)
        probe = run.seen[0]
        self.assertEqual(probe[:2], ['bazel', 'aquery'])
        self.assertIn('--//boot:erista_boot_run=false', probe)
        self.assertIn('--jobs=4', probe)
        self.assertIn('mnemonic("BootScript|BoardHeader|BoardAgree", //boot:erista_first_xnu_seeded)', probe)
        for flag in erista_seed.CACHE_FLAGS + erista_seed.BUILD_FLAGS:
            self.assertIn(flag, probe)

    def test_main_builds_nothing_when_a_check_fails(self):
        for failure in (
            dict(argv=['build', 'seed', '--', '--//boot:erista_boot_run=true']),
            dict(argv=['build', 'seed'], rc=wrapper.BuildError('rc names erista_boot_run')),
            dict(argv=['build', 'seed'], probe=wrapper.BuildError('derived')),
        ):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as scratch, mock.patch.object(
                wrapper.erista_seed, 'load', return_value=bytes(range(256))
            ), mock.patch.object(wrapper, 'git_clean'), mock.patch.object(
                wrapper, 'rc_files_clean', side_effect=failure.get('rc')
            ), mock.patch.object(wrapper, 'boot_row_is_off', side_effect=failure.get('probe')), mock.patch.object(
                wrapper.subprocess, 'run'
            ) as launched:
                self.assertEqual(1, wrapper.main(failure['argv']))
                launched.assert_not_called()

    def test_main_builds_once_when_every_check_passes(self):
        with mock.patch.object(wrapper.erista_seed, 'load', return_value=bytes(range(256))), mock.patch.object(
            wrapper, 'git_clean'
        ), mock.patch.object(wrapper, 'rc_files_clean'), mock.patch.object(wrapper, 'boot_row_is_off'), mock.patch.object(
            wrapper.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)
        ) as launched:
            self.assertEqual(0, wrapper.main(['build', 'seed']))
            self.assertEqual(1, launched.call_count)
            self.assertEqual(launched.call_args.args[0][-2], '--//boot:erista_boot_run=false')

    def test_only_the_flag_its_condition_and_the_two_targets_name_it(self):
        self.assertNotIn('erista_boot_run', DEFS)
        lines = [line.strip() for line in BUILD.splitlines() if 'erista_boot_run' in line and not line.strip().startswith('#')]
        self.assertTrue(lines)
        for line in lines:
            self.assertRegex(line, r'name = "erista_boot_run(_on)?",|":erista_boot_run(_on)?"')


if __name__ == '__main__':
    unittest.main()
