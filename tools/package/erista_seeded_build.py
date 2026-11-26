"""Builds, scans and removes the per-install-seed package (ERISTA_RANDOM_SEED.md).

    erista_seeded_build.py build [PATH]    check the seed, git, the output base and the action graph, then bazel build with every cache off
    erista_seeded_build.py actions [PATH]  the action-graph check alone: which actions hold the seed and whether each is uncacheable
    erista_seeded_build.py scan [PATH]     look for the seed in git's tracked files, in --also files and directories, and with --caches in
                                           the disk caches and bazel's logs; the report lists what was and was not covered
    erista_seeded_build.py clean [PATH]    delete the seeded outputs; --seed also the seed file; --deep also runs `bazel clean`

The seed is never printed and never passed on a command line: bazel gets only its path, through //local.
The package it builds is the boot=0 one: the flag that lets the Erista boot (//boot:erista_boot_run) is refused in every
spelling this tool can see, forced off on the command line, and the configuration bazel ends up with is analysed first.
"""

import argparse
import os
import re
import stat
import subprocess
import sys
from pathlib import Path

import erista_seed
import seed_places
import seed_scan
import seeded_taint

BOOT_RUN_FLAG = 'erista_boot_run'
BOOT_RUN_OFF = '--//boot:erista_boot_run=false'
# Options that pull in flags this tool cannot read: another config, an rc or flag file.
INDIRECT = ('--config', '--bazelrc', '--flagfile', '--flag_alias', '--noworkspace_rc', '--nohome_rc', '--noworkspace')
BOOT_ROW_ACTIONS = ('BoardHeader', 'BoardAgree')
# In every package, so a query that does not list it looked at nothing and proves nothing.
CONTROL_ACTION = 'BootScript'
# Whatever else is passed to bazel must be on this list. Cache, remote, build-event and rc flags are never on it.
ALLOWED_EXTRA = (
    '--jobs',
    '--keep_going',
    '--color',
    '--curses',
    '--progress_in_terminal_title',
    '--show_progress',
    '--show_timestamps',
    '--local_cpu_resources',
    '--local_ram_resources',
    '--announce_rc',
    '--nobuild',
    '-k',
)
# Written as --name=value: a bare one would take the next word, a cache flag, as its value.
TAKES_VALUE = ('--jobs', '--color', '--curses', '--local_cpu_resources', '--local_ram_resources')
SENDS_OR_CACHES = re.compile(r'remote|disk_cache|bes_|build_event|grpc|credential|google_', re.I)
FORBIDDEN = (
    '-s',
    '--subcommands',
    '--verbose_failures',
    '--sandbox_debug',
    '--explain',
    '--build_event_json_file',
    '--build_event_text_file',
    '--build_event_binary_file',
    '--profile',
    '--execution_log_json_file',
    '--execution_log_binary_file',
)


class BuildError(Exception):
    pass


def git_clean(path: Path, run=subprocess.run) -> None:
    """Refuse a seed git could pick up: tracked, or not ignored."""
    tracked = run(['git', 'ls-files', '--error-unmatch', str(path)], capture_output=True, check=False)
    if tracked.returncode == 0:
        raise BuildError(f'{path} is tracked by git')
    ignored = run(['git', 'check-ignore', '-q', str(path)], check=False)
    if ignored.returncode != 0:
        raise BuildError(f'{path} is not git-ignored')


def allowed(flag: str) -> bool:
    name = flag.split('=')[0]
    if name in TAKES_VALUE and '=' not in flag:
        return False
    return name in ALLOWED_EXTRA or (name.startswith('--no') and '--' + name[4:] in ALLOWED_EXTRA)


def bazel_command(extra=(), verb: str = 'build') -> list[str]:
    bad = [flag for flag in extra if flag.split('=')[0] in FORBIDDEN]
    if bad:
        raise BuildError(f'{bad[0]} would put build details in a log or file that outlives the seed')
    for flag in extra:
        if BOOT_RUN_FLAG in flag.lower():
            raise BuildError(f'{flag}: the Erista boot permission is not approved and this tool never builds with it')
        if flag.split('=')[0] in INDIRECT or flag.startswith('@'):
            raise BuildError(f'{flag} could bring in flags this tool cannot check')
        if SENDS_OR_CACHES.search(flag):
            raise BuildError(f'{flag} could turn a cache or an upload back on')
        if not allowed(flag):
            raise BuildError(f'{flag} is not on the list of flags this tool passes on: {", ".join(ALLOWED_EXTRA)}')
    # The cache flags and the forced-off flag come last: for a flag given twice bazel takes the later one, so nothing
    # earlier on the line (the config, an rc file, an extra flag) can undo them.
    return ['bazel', verb, *erista_seed.BUILD_FLAGS, *extra, *erista_seed.CACHE_FLAGS, BOOT_RUN_OFF, erista_seed.TARGET]


def rc_files(workspace: Path = Path(), home: Path | None = None) -> list[Path]:
    return [workspace / '.bazelrc', workspace / 'user.bazelrc', (home or Path.home()) / '.bazelrc']


def rc_files_clean(files: list[Path]) -> None:
    for file in files:
        if file.is_file() and BOOT_RUN_FLAG in file.read_text(errors='replace').lower():
            raise BuildError(f'{file} names {BOOT_RUN_FLAG}: remove it before a seeded build')


def boot_row_is_off(extra=(), run=subprocess.run) -> None:
    """Analyse the package with the command line the build will use: it must contain no derived board table."""
    command = bazel_command(extra, 'aquery')
    command = [
        *command[:-1],
        f'mnemonic("{"|".join((CONTROL_ACTION, *BOOT_ROW_ACTIONS))}", {command[-1]})',
        '--output=text',
    ]
    done = run(command, capture_output=True, text=True, check=False)
    if done.returncode != 0 or f'Mnemonic: {CONTROL_ACTION}' not in done.stdout:
        raise BuildError('could not analyse the package to check that the Erista boot row is off')
    found = [name for name in BOOT_ROW_ACTIONS if f'Mnemonic: {name}' in done.stdout]
    if found:
        raise BuildError(
            f'the package would carry {found[0]}: the Erista boot row is on, and this tool does not build that'
        )


def tracked_files(run=subprocess.run) -> list[Path]:
    listing = run(['git', 'ls-files', '-z'], capture_output=True, check=True).stdout
    return [Path(name.decode()) for name in listing.split(b'\0') if name]


def output_base_private(run=subprocess.run, uid: int | None = None) -> Path:
    """The seeded outputs sit in bazel's output base with other users' read permission unless the base itself is closed."""
    base = seed_places.bazel_info('output_base', run)
    info = os.stat(base)
    mode = stat.S_IMODE(info.st_mode)
    if info.st_uid != (os.getuid() if uid is None else uid):
        raise BuildError(f'{base} belongs to another user')
    if mode & 0o077:
        raise BuildError(
            f'{base} has mode {mode:o}, so other users on this Mac can read the seeded outputs: chmod 700 "{base}"'
        )
    acl = run(['ls', '-led', str(base)], capture_output=True, text=True, check=False)
    if acl.returncode == 0 and re.search(r'(?m)^\s*\d+: ', acl.stdout):
        raise BuildError(f'{base} has an access control list: remove it (chmod -N "{base}") before a seeded build')
    return base


def clean(bazel_bin: Path, seed_path: Path | None) -> list[Path]:
    """Only the seeded outputs under bazel-bin/boot; the ordinary packages and everything else stay."""
    removed = []
    for name in seed_places.OUTPUTS:
        target = bazel_bin / 'boot' / name
        if target.exists():
            subprocess.run(['chmod', '-R', 'u+w', str(target)], check=False)
            subprocess.run(['rm', '-rf', str(target)], check=True)
            removed.append(target)
    if seed_path and seed_path.exists():
        seed_path.unlink()
        removed.append(seed_path)
    return removed


def tracked_existing(run=subprocess.run) -> list[Path]:
    return [p for p in tracked_files(run) if os.path.lexists(p)]


def scan_places(args, run=subprocess.run) -> tuple[list[Path], list[Path], list[str]]:
    """Leak places, expected places and what was left out, by choice or because the place does not exist."""
    leak = tracked_existing(run) + list(args.also)
    expected, notes = [], []
    if args.caches:
        caches = seed_places.disk_caches([*rc_files(), Path('/etc/bazel.bazelrc')])
        notes += [
            f'disk cache {c}: ' + ('scanned' if c.exists() else 'does not exist, nothing cached there') for c in caches
        ]
        leak += [c for c in caches if c.exists()]
        if not caches:
            notes.append('no disk cache named in an rc file')
        base = seed_places.bazel_info('output_base', run)
        places, excluded = seed_places.output_base_places(base)
        leak += places
        notes += excluded
        expected = seed_places.seeded_outputs(seed_places.bazel_info('bazel-bin', run))
    return leak, expected, notes


def run_scan(seed: bytes, args, run=subprocess.run) -> int:
    leak, expected, notes = scan_places(args, run)
    try:
        report = seed_scan.scan(seed, leak, expected)
    except seed_scan.ScanError as exc:
        raise BuildError(str(exc)) from exc
    for path in report.leaks:
        print(f'seed found in {path}', file=sys.stderr)
    for path in report.unreadable:
        print(f'could not read {path}', file=sys.stderr)
    print(*notes, *seed_scan.summary(report), 'remote caches and the SD card are not covered', sep='\n')
    control = [p for p in expected if p.name == seed_places.CONTROL]
    if control and control[0] not in report.expected_hits:
        raise BuildError('the scan did not find the seed in the seeded ADT, which holds it: this scan proves nothing')
    print('no leak found' if report.clean else 'leak or unreadable file: see above')
    return 0 if report.clean else 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('build', 'actions', 'scan', 'clean'))
    parser.add_argument('path', nargs='?', type=Path, default=erista_seed.DEFAULT_PATH)
    parser.add_argument(
        '--also', type=Path, action='append', default=[], help='scan: more files or directories, such as a saved log'
    )
    parser.add_argument(
        '--caches', action='store_true', help="scan: the disk caches named in rc files and bazel's logs and state"
    )
    parser.add_argument('--seed', action='store_true', help='clean: delete the seed file too')
    parser.add_argument(
        '--deep', action='store_true', help='clean: also `bazel clean`, which removes every build output'
    )
    parser.add_argument(
        'bazel_args', nargs='*', help='build, actions: more bazel flags from the allowed list, after --'
    )
    args = parser.parse_args(argv)
    try:
        if args.mode in ('build', 'actions'):
            seed = erista_seed.load(args.path)
            git_clean(args.path)
            command = bazel_command(args.bazel_args)
            rc_files_clean(rc_files())
            if args.mode == 'build':
                output_base_private()
            boot_row_is_off(args.bazel_args)
            counts = seeded_taint.seed_actions_are_private(args.bazel_args)
            print(
                f'seed ok (fingerprint {erista_seed.fingerprint(seed)}); {sum(counts.values())} actions hold the seed, all uncacheable:'
            )
            print(*(f'  {name}: {count}' for name, count in counts.items()), sep='\n')
            if args.mode == 'actions':
                return 0
            print('building with every cache off')
            return subprocess.run(command, check=False).returncode
        if args.mode == 'scan':
            return run_scan(erista_seed.load(args.path), args)
        info = subprocess.run(['bazel', 'info', 'bazel-bin'], capture_output=True, text=True, check=True)
        for removed in clean(Path(info.stdout.strip()), args.path if args.seed else None):
            print(f'removed {removed}')
        if args.deep:
            return subprocess.run(['bazel', 'clean'], check=False).returncode
        return 0
    except (erista_seed.SeedError, BuildError, seeded_taint.TaintError, OSError) as exc:
        print(f'refused: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
