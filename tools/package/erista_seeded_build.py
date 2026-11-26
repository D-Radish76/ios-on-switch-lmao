"""Builds, scans and removes the per-install-seed package (ERISTA_RANDOM_SEED.md).

    erista_seeded_build.py build [PATH]    check the seed and git, then bazel build with every cache off
    erista_seeded_build.py scan [PATH]     look for the seed in git's tracked files and in the files named by --also
    erista_seeded_build.py clean [PATH]    delete the seeded outputs and, with --seed, the seed file

The seed is never printed and never passed on a command line: bazel gets only its path, through //local.
The package it builds is the boot=0 one: the flag that lets the Erista boot (//boot:erista_boot_run) is refused in every
spelling this tool can see, forced off on the command line, and the configuration bazel ends up with is analysed first.
"""

import argparse
import subprocess
import sys
from pathlib import Path

import erista_seed

BOOT_RUN_FLAG = 'erista_boot_run'
BOOT_RUN_OFF = '--//boot:erista_boot_run=false'
# Options that pull in flags this tool cannot read: another config, an rc or flag file.
INDIRECT = ('--config', '--bazelrc', '--flagfile', '--flag_alias', '--noworkspace_rc', '--nohome_rc', '--noworkspace')
BOOT_ROW_ACTIONS = ('BoardHeader', 'BoardAgree')
# In every package, so a query that does not list it looked at nothing and proves nothing.
CONTROL_ACTION = 'BootScript'
OUTPUTS = ('erista_first_xnu_seeded_work', 'erista-first-xnu-seeded')
FORBIDDEN = ('-s', '--subcommands', '--verbose_failures', '--sandbox_debug', '--explain', '--build_event_json_file',
             '--build_event_text_file', '--build_event_binary_file', '--profile', '--execution_log_json_file',
             '--execution_log_binary_file')


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


def bazel_command(extra=(), verb: str = 'build') -> list[str]:
    bad = [flag for flag in extra if flag.split('=')[0] in FORBIDDEN]
    if bad:
        raise BuildError(f'{bad[0]} would put build details in a log or file that outlives the seed')
    for flag in extra:
        if BOOT_RUN_FLAG in flag.lower():
            raise BuildError(f'{flag}: the Erista boot permission is not approved and this tool never builds with it')
        if flag.split('=')[0] in INDIRECT or flag.startswith('@'):
            raise BuildError(f'{flag} could bring in flags this tool cannot check')
    # Last on the line, so it also wins over an rc file or a flag the checks above did not see.
    return ['bazel', verb, *erista_seed.BUILD_FLAGS, *erista_seed.CACHE_FLAGS, *extra, BOOT_RUN_OFF, erista_seed.TARGET]


def rc_files(workspace: Path = Path(), home: Path | None = None) -> list[Path]:
    return [workspace / '.bazelrc', workspace / 'user.bazelrc', (home or Path.home()) / '.bazelrc']


def rc_files_clean(files: list[Path]) -> None:
    for file in files:
        if file.is_file() and BOOT_RUN_FLAG in file.read_text(errors='replace').lower():
            raise BuildError(f'{file} names {BOOT_RUN_FLAG}: remove it before a seeded build')


def boot_row_is_off(extra=(), run=subprocess.run) -> None:
    """Analyse the package with the command line the build will use: it must contain no derived board table."""
    command = bazel_command(extra, 'aquery')
    command = [*command[:-1], f'mnemonic("{"|".join((CONTROL_ACTION, *BOOT_ROW_ACTIONS))}", {command[-1]})', '--output=text']
    done = run(command, capture_output=True, text=True, check=False)
    if done.returncode != 0 or f'Mnemonic: {CONTROL_ACTION}' not in done.stdout:
        raise BuildError('could not analyse the package to check that the Erista boot row is off')
    found = [name for name in BOOT_ROW_ACTIONS if f'Mnemonic: {name}' in done.stdout]
    if found:
        raise BuildError(f'the package would carry {found[0]}: the Erista boot row is on, and this tool does not build that')


def scan(seed: bytes, files: list[Path]) -> list[Path]:
    """The files that hold the seed, as hex text or raw bytes (its middle 64 bytes, which the pinned seed does not share)."""
    needles = (seed[96:160], seed[96:160].hex().encode())
    return [p for p in files if p.is_file() and any(n in p.read_bytes() for n in needles)]


def tracked_files(run=subprocess.run) -> list[Path]:
    listing = run(['git', 'ls-files', '-z'], capture_output=True, check=True).stdout
    return [Path(name.decode()) for name in listing.split(b'\0') if name]


def clean(bazel_bin: Path, seed_path: Path | None) -> list[Path]:
    removed = []
    for name in OUTPUTS:
        target = bazel_bin / 'boot' / name
        if target.exists():
            subprocess.run(['chmod', '-R', 'u+w', str(target)], check=False)
            subprocess.run(['rm', '-rf', str(target)], check=True)
            removed.append(target)
    if seed_path and seed_path.exists():
        seed_path.unlink()
        removed.append(seed_path)
    return removed


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('build', 'scan', 'clean'))
    parser.add_argument('path', nargs='?', type=Path, default=erista_seed.DEFAULT_PATH)
    parser.add_argument('--also', type=Path, action='append', default=[], help='scan: more files, such as a saved log')
    parser.add_argument('--seed', action='store_true', help='clean: delete the seed file too')
    parser.add_argument('bazel_args', nargs='*', help='build: more bazel flags, after --')
    args = parser.parse_args(argv)
    try:
        if args.mode == 'build':
            seed = erista_seed.load(args.path)
            git_clean(args.path)
            command = bazel_command(args.bazel_args)
            rc_files_clean(rc_files())
            boot_row_is_off(args.bazel_args)
            print(f'seed ok (fingerprint {erista_seed.fingerprint(seed)}); building with every cache off')
            return subprocess.run(command, check=False).returncode
        if args.mode == 'scan':
            leaks = scan(erista_seed.load(args.path), tracked_files() + args.also)
            for leak in leaks:
                print(f'seed found in {leak}', file=sys.stderr)
            print('no leak found' if not leaks else f'{len(leaks)} file(s) hold the seed')
            return 1 if leaks else 0
        info = subprocess.run(['bazel', 'info', 'bazel-bin'], capture_output=True, text=True, check=True)
        for removed in clean(Path(info.stdout.strip()), args.path if args.seed else None):
            print(f'removed {removed}')
        return 0
    except (erista_seed.SeedError, BuildError) as exc:
        print(f'refused: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
