"""Builds, scans and removes the per-install-seed package (ERISTA_RANDOM_SEED.md).

    erista_seeded_build.py build [PATH]    check the seed and git, then bazel build with every cache off
    erista_seeded_build.py scan [PATH]     look for the seed in git's tracked files and in the files named by --also
    erista_seeded_build.py clean [PATH]    delete the seeded outputs and, with --seed, the seed file

The seed is never printed and never passed on a command line: bazel gets only its path, through //local.
"""

import argparse
import subprocess
import sys
from pathlib import Path

import erista_seed

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


def bazel_command(extra=()) -> list[str]:
    bad = [flag for flag in extra if flag.split('=')[0] in FORBIDDEN]
    if bad:
        raise BuildError(f'{bad[0]} would put build details in a log or file that outlives the seed')
    return ['bazel', 'build', *erista_seed.BUILD_FLAGS, *erista_seed.CACHE_FLAGS, *extra, erista_seed.TARGET]


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
