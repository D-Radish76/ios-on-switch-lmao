"""The seeded build's wrapper and the cache promises behind it: what it refuses, what it passes to bazel, what its scan finds."""

import os
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
        for option in (
            '--config=x',
            '--bazelrc=x',
            '--flagfile=x',
            '--flag_alias=a=//b:c',
            '--noworkspace_rc',
            '@args.txt',
        ):
            with self.subTest(option=option), self.assertRaises(wrapper.BuildError):
                wrapper.bazel_command([option])

    def test_the_command_ends_with_the_flag_forced_off_and_the_target(self):
        command = wrapper.bazel_command(['--jobs=4'])
        self.assertEqual(command[-2:], ['--//boot:erista_boot_run=false', '//boot:erista_first_xnu_seeded'])
        self.assertEqual(command[-2 - len(erista_seed.CACHE_FLAGS) : -2], list(erista_seed.CACHE_FLAGS))
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
        self.assertLess(probe.index('--jobs=4'), probe.index('--remote_cache='))
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
            with (
                self.subTest(failure=failure),
                tempfile.TemporaryDirectory() as scratch,
                mock.patch.object(wrapper.erista_seed, 'load', return_value=bytes(range(256))),
                mock.patch.object(wrapper, 'git_clean'),
                mock.patch.object(wrapper, 'rc_files_clean', side_effect=failure.get('rc')),
                mock.patch.object(wrapper, 'boot_row_is_off', side_effect=failure.get('probe')),
                mock.patch.object(wrapper, 'output_base_private'),
                mock.patch.object(wrapper.seeded_taint, 'seed_actions_are_private', return_value={'BootAdt': 1}),
                mock.patch.object(wrapper.subprocess, 'run') as launched,
            ):
                self.assertEqual(1, wrapper.main(failure['argv']))
                launched.assert_not_called()

    def test_main_builds_once_when_every_check_passes(self):
        with (
            mock.patch.object(wrapper.erista_seed, 'load', return_value=bytes(range(256))),
            mock.patch.object(wrapper, 'git_clean'),
            mock.patch.object(wrapper, 'rc_files_clean'),
            mock.patch.object(wrapper, 'boot_row_is_off'),
            mock.patch.object(wrapper, 'output_base_private'),
            mock.patch.object(wrapper.seeded_taint, 'seed_actions_are_private', return_value={'BootAdt': 1}),
            mock.patch.object(wrapper.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as launched,
        ):
            self.assertEqual(0, wrapper.main(['build', 'seed']))
            self.assertEqual(1, launched.call_count)
            self.assertEqual(launched.call_args.args[0][-2], '--//boot:erista_boot_run=false')

    def test_only_the_flag_its_condition_and_the_two_targets_name_it(self):
        self.assertNotIn('erista_boot_run', DEFS)
        lines = [
            line.strip()
            for line in BUILD.splitlines()
            if 'erista_boot_run' in line and not line.strip().startswith('#')
        ]
        self.assertTrue(lines)
        for line in lines:
            self.assertRegex(line, r'name = "erista_boot_run(_on)?",|":erista_boot_run(_on)?"')


class FixedSetMatchesTheRule(unittest.TestCase):
    def test_the_shard_count_the_check_demands_is_the_one_the_rule_creates(self):
        shards = int(re.search(r'^_WINDOW_SHARDS = (\d+)$', DEFS, re.M).group(1))
        self.assertEqual(wrapper.seeded_taint.FIXED_SETS['KernelCompress'], shards)
        self.assertEqual(len(re.findall(r'mnemonic = "KernelCompress"', DEFS)), 1)


class CachesStayOff(unittest.TestCase):
    def test_the_cache_flags_come_after_everything_a_caller_or_a_config_can_set(self):
        command = wrapper.bazel_command(['--jobs=4', '--keep_going'])
        last_free = max(command.index(f) for f in ('--config=ios17', '--jobs=4', '--keep_going'))
        for flag in erista_seed.CACHE_FLAGS:
            self.assertGreater(command.index(flag), last_free)

    def test_the_build_event_flags_are_off_too(self):
        for flag in ('--bes_backend=', '--bes_results_url='):
            self.assertIn(flag, erista_seed.CACHE_FLAGS)

    def test_flags_that_would_turn_a_cache_or_an_upload_back_on_are_refused(self):
        for flag in (
            '--remote_cache=grpc://cache:1',
            '--noremote_upload_local_results=false',
            '--remote_executor=grpc://x',
            '--disk_cache=/tmp/x',
            '--experimental_disk_cache_gc_max_size=1G',
            '--bes_backend=grpc://x',
            '--bes_results_url=https://x',
            '--experimental_remote_downloader=x',
            '--google_default_credentials',
            '--remote_accept_cached',
            '--REMOTE_CACHE=x',
            '--grpc_keepalive_time=1s',
        ):
            with self.subTest(flag=flag), self.assertRaises(wrapper.BuildError):
                wrapper.bazel_command([flag])

    def test_a_flag_that_is_not_on_the_list_is_refused_and_a_listed_one_passes(self):
        for flag in (
            '--copt=-O0',
            '--action_env=X=1',
            '--sandbox_base=/tmp',
            'extra_target',
            '--jobs',
            '-j4',
            '//other:target',
        ):
            with self.subTest(flag=flag), self.assertRaises(wrapper.BuildError):
                wrapper.bazel_command([flag])
        for flag in ('--jobs=4', '--keep_going', '--nokeep_going', '--color=no', '--announce_rc', '-k'):
            with self.subTest(flag=flag):
                self.assertIn(flag, wrapper.bazel_command([flag]))

    def test_an_rc_file_cannot_move_a_cache_flag_behind_the_command_line(self):
        # rc and config flags are expanded where --config stands, which is before every cache flag.
        command = wrapper.bazel_command()
        self.assertLess(command.index('--config=ios17'), command.index('--disk_cache='))
        self.assertLess(command.index('--config=ios17'), command.index('--bes_backend='))


class CleanReach(unittest.TestCase):
    def test_clean_removes_the_seeded_outputs_only(self):
        with tempfile.TemporaryDirectory() as scratch:
            boot = Path(scratch) / 'boot'
            for name in wrapper.seed_places.OUTPUTS:
                target = boot / name
                if name.endswith('.adt'):
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(b'x')
                else:
                    (target / 'deep').mkdir(parents=True)
                    (target / 'deep' / 'f').write_bytes(b'x')
            keep = [
                boot / 'erista_first_xnu_work',
                boot / 'erista-first-xnu',
                boot / 'erista-full-ram.adt',
                boot / 'erista_package',
            ]
            for path in keep:
                path.mkdir(exist_ok=True) if path.name != 'erista-full-ram.adt' else path.write_bytes(b'x')
            seed = Path(scratch) / 'seed'
            seed.write_text('x')
            removed = wrapper.clean(Path(scratch), None)
            self.assertEqual({p.name for p in removed}, set(wrapper.seed_places.OUTPUTS))
            self.assertTrue(all(p.exists() for p in keep))
            self.assertTrue(seed.exists())
            self.assertEqual([seed], wrapper.clean(Path(scratch), seed))
            self.assertFalse(seed.exists())

    def test_the_seeded_adt_is_among_the_outputs(self):
        self.assertIn('erista-full-ram-seeded.adt', wrapper.seed_places.OUTPUTS)

    def test_deep_is_an_explicit_option_and_runs_only_bazel_clean(self):
        calls = []

        def run(command, **_):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, '/nowhere\n', '')

        with mock.patch.object(wrapper.subprocess, 'run', run):
            self.assertEqual(0, wrapper.main(['clean']))
            self.assertNotIn(['bazel', 'clean'], calls)
            self.assertEqual(0, wrapper.main(['clean', '--deep']))
            self.assertEqual(['bazel', 'clean'], calls[-1])
            self.assertNotIn('--expunge', calls[-1])


class OutputBase(unittest.TestCase):
    def check(self, mode, acl='', uid=None):
        with tempfile.TemporaryDirectory() as scratch:
            base = Path(scratch) / 'ob'
            base.mkdir()
            base.chmod(mode)

            def run(command, **_):
                if command[:2] == ['bazel', 'info']:
                    return subprocess.CompletedProcess(command, 0, f'{base}\n', '')
                return subprocess.CompletedProcess(command, 0 if acl else 1, acl, '')

            return wrapper.output_base_private(run, uid)

    def test_a_private_output_base_passes(self):
        self.check(0o700)

    def test_an_output_base_others_can_enter_or_read_is_refused_with_the_fix(self):
        for mode in (0o755, 0o750, 0o705, 0o711):
            with self.subTest(mode=oct(mode)), self.assertRaises(wrapper.BuildError) as raised:
                self.check(mode)
            self.assertIn('chmod 700', str(raised.exception))

    def test_an_access_control_list_is_refused(self):
        with self.assertRaises(wrapper.BuildError):
            self.check(0o700, acl='drwx------+ 5 u staff 160 x ob\n 0: group:everyone allow list\n')

    def test_another_users_output_base_is_refused(self):
        with self.assertRaises(wrapper.BuildError):
            self.check(0o700, uid=os.getuid() + 1)

    def test_a_failed_query_stops_the_build(self):
        with self.assertRaises(OSError):
            wrapper.output_base_private(lambda c, **_: subprocess.CompletedProcess(c, 1, '', ''))


class ScanReach(unittest.TestCase):
    SEED = bytes(range(256))[::-1]

    def args(self, *, also=(), caches=False, allow=False):
        return type('Args', (), {'also': list(also), 'caches': caches, 'allow_no_control': allow})()

    def test_a_missing_path_is_an_error_and_a_clean_directory_passes(self):
        with tempfile.TemporaryDirectory() as scratch, mock.patch.object(wrapper, 'tracked_existing', return_value=[]):
            self.assertEqual(0, wrapper.run_scan(self.SEED, self.args(also=[Path(scratch)])))
            with self.assertRaises(wrapper.BuildError):
                wrapper.run_scan(self.SEED, self.args(also=[Path(scratch) / 'nope']))

    def test_a_leak_in_a_nested_directory_fails_and_the_seed_is_never_printed(self):
        import contextlib
        import io

        with tempfile.TemporaryDirectory() as scratch, mock.patch.object(wrapper, 'tracked_existing', return_value=[]):
            nested = Path(scratch) / 'a' / 'b'
            nested.mkdir(parents=True)
            (nested / 'cache-blob').write_bytes(b'\0' * 9 + self.SEED)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(1, wrapper.run_scan(self.SEED, self.args(also=[Path(scratch)])))
            shown = out.getvalue() + err.getvalue()
            self.assertIn(str(nested / 'cache-blob'), shown)
            self.assertNotIn(self.SEED[96:160].hex(), shown)
            self.assertIn('not detected', shown)
            self.assertIn('scanned 1 files', shown)

    def test_caches_adds_rc_disk_caches_and_output_base_state_and_skips_the_seeded_outputs(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            base, binary, cache = root / 'ob', root / 'bin', root / 'cache'
            for directory in (
                base / 'execroot',
                base / 'external',
                base / 'action_cache',
                base / 'sandbox',
                cache,
                binary / 'boot',
            ):
                directory.mkdir(parents=True)
            (base / 'command.log').write_text('clean')
            (base / 'execroot' / 'seeded').write_bytes(self.SEED)
            (cache / 'blob').write_bytes(b'ok')
            (binary / 'boot' / 'erista-full-ram-seeded.adt').write_bytes(b'adt' + self.SEED)

            def run(command, **_):
                value = {'output_base': base, 'bazel-bin': binary}[command[2]]
                return subprocess.CompletedProcess(command, 0, f'{value}\n', '')

            with (
                mock.patch.object(wrapper, 'tracked_existing', return_value=[]),
                mock.patch.object(wrapper.seed_places, 'disk_caches', return_value=[cache, root / 'gone']),
            ):
                self.assertEqual(0, wrapper.run_scan(self.SEED, self.args(caches=True), run))

    def test_a_scan_whose_control_misses_the_seeded_adt_fails(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            base, binary = root / 'ob', root / 'bin'
            (binary / 'boot').mkdir(parents=True)
            base.mkdir()
            (binary / 'boot' / 'erista-full-ram-seeded.adt').write_bytes(b'no seed here')

            def run(command, **_):
                return subprocess.CompletedProcess(
                    command, 0, f'{ {"output_base": base, "bazel-bin": binary}[command[2]] }\n', ''
                )

            with (
                mock.patch.object(wrapper, 'tracked_existing', return_value=[]),
                mock.patch.object(wrapper.seed_places, 'disk_caches', return_value=[]),
            ):
                with self.assertRaises(wrapper.BuildError):
                    wrapper.run_scan(self.SEED, self.args(caches=True), run)

    def test_disk_caches_are_read_from_every_rc_config_and_comments_are_skipped(self):
        with tempfile.TemporaryDirectory() as scratch:
            rc = Path(scratch) / 'rc'
            rc.write_text(
                'common --disk_cache=~/Library/Caches/x/disk\n# common --disk_cache=/never\nbuild:reproducible-check --disk_cache=\n'
                'build:remote --disk_cache="/tmp/other"\n'
            )
            found = wrapper.seed_places.disk_caches([rc, Path(scratch) / 'missing'])
            self.assertEqual([Path.home() / 'Library/Caches/x/disk', Path('/tmp/other')], found)


class ControlOutcome(unittest.TestCase):
    SEED = bytes(range(256))[::-1]

    def args(self, **kw):
        values = {'also': [], 'caches': True, 'allow_no_control': False, **kw}
        return type('Args', (), values)()

    def environment(self, scratch, adt=b'adt' + SEED, leak=False):
        root = Path(scratch)
        base, binary = root / 'ob', root / 'bin'
        (binary / 'boot').mkdir(parents=True)
        base.mkdir()
        if adt is not None:
            (binary / 'boot' / 'erista-full-ram-seeded.adt').write_bytes(adt)
        if leak:
            (base / 'command.log').write_bytes(self.SEED)

        def run(command, **_):
            return subprocess.CompletedProcess(
                command, 0, f'{ {"output_base": base, "bazel-bin": binary}[command[2]] }\n', ''
            )

        return run

    def scan(self, **kw):
        import contextlib
        import io

        out = io.StringIO()
        with (
            tempfile.TemporaryDirectory() as scratch,
            mock.patch.object(wrapper, 'tracked_existing', return_value=[]),
            mock.patch.object(wrapper.seed_places, 'disk_caches', return_value=[]),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            run = self.environment(scratch, **{k: kw[k] for k in ('adt', 'leak') if k in kw})
            try:
                code = wrapper.run_scan(
                    self.SEED, self.args(**{k: v for k, v in kw.items() if k not in ('adt', 'leak')}), run
                )
            except wrapper.BuildError as exc:
                code = f'refused: {exc}'
        return code, out.getvalue()

    def test_a_passed_control_is_printed_and_exits_0(self):
        code, shown = self.scan()
        self.assertEqual(code, 0)
        self.assertIn('control: PASSED', shown)
        self.assertIn('no leak found', shown)

    def test_a_missing_adt_is_not_a_pass_and_stops_the_final_check(self):
        code, _ = self.scan(adt=None)
        self.assertRegex(str(code), 'refused: control missing')

    def test_after_a_clean_the_result_is_distinct_and_says_it_is_not_verified(self):
        code, shown = self.scan(adt=None, allow_no_control=True)
        self.assertEqual(code, 2)
        self.assertIn('control: NOT VERIFIED', shown)
        self.assertIn('no leak found, control not verified', shown)
        self.assertNotIn('control: PASSED', shown)

    def test_a_failed_control_stops_even_when_nothing_leaked(self):
        code, _ = self.scan(adt=b'nothing')
        self.assertRegex(str(code), 'refused: control FAILED')
        code, _ = self.scan(adt=b'nothing', allow_no_control=True)
        self.assertRegex(str(code), 'refused: control FAILED')

    def test_a_leak_is_exit_1_whatever_the_control_says(self):
        self.assertEqual(self.scan(leak=True)[0], 1)
        self.assertEqual(self.scan(leak=True, adt=None, allow_no_control=True)[0], 1)

    def test_a_git_only_scan_says_it_ran_no_control(self):
        code, shown = self.scan(caches=False)
        self.assertEqual(code, 0)
        self.assertIn('control: not run', shown)
        self.assertNotIn('PASSED', shown)

    def test_the_flag_exists_and_the_default_requires_the_control(self):
        parser_source = (ROOT / 'tools/package/erista_seeded_build.py').read_text()
        self.assertIn("'--allow-no-control'", parser_source)


if __name__ == '__main__':
    unittest.main()
