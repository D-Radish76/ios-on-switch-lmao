"""The Erista's per-install random seed (ERISTA_RANDOM_SEED.md, option C).

    erista_seed.py generate [PATH] [--force]   write 256 random bytes as hex, mode 0600, from the Mac's secure RNG
    erista_seed.py check [PATH]                exit 0 only for a seed that is set, private and not the pinned one
    erista_seed.py command [PATH]              the bazel command for the seeded package, with the caches off

The seed is never printed. PATH defaults to local/erista-seed.hex, which git ignores.
"""

import hashlib
import os
import secrets
import stat
import sys
from pathlib import Path

SEED_BYTES = 256
DEFAULT_PATH = Path('local/erista-seed.hex')
# //boot's _ADT_SEED: public, in the repository. A seed equal to it, or to its first 64 bytes, is not a per-install one.
PINNED_SEED = bytes.fromhex(
    '41d4ed8aee6621528a6a355e703603425eef27f7a8df3c8f3cf4460c943df40e4c8b6d7631620a39969418a79e0cb0817cd0d525a13134285ee356ac8871a940'
    'bf7c5ca763a3b3b59a6396a4a5a1a65eb6be4e7192b809aa9472e695a17d5e6492afe92ba9f0d4c9d9831c3c7e4de329dc1d45cc8860982c20d89f67a6bc986'
    '3f4d1e123b269c92f815d8dc4c3cd4465fd94347865f56697cabaace0f626ee9eff9073be0c618df269f828ab451859f0812764250cb8cfcf352750e55b3b67'
    '3a72be976026a8d60fb7182acd76062b23e21bd37f0bbe69e432da1b811cb23fc49a56466fb359c24c2d1cc5bcd9c0fc9c93d1f8f00bac4d25095a25181dde4696'
)
MIN_DISTINCT_BYTES = 100


class SeedError(Exception):
    pass


def parse(text: str) -> bytes:
    try:
        seed = bytes.fromhex(text.strip())
    except ValueError as exc:
        raise SeedError('the seed file is not hex') from exc
    if len(seed) != SEED_BYTES:
        raise SeedError(f'the seed must be {SEED_BYTES} bytes, not {len(seed)}')
    if seed == PINNED_SEED or seed[:64] == PINNED_SEED[:64]:
        raise SeedError('the seed is the pinned, public one')
    if len(set(seed)) < MIN_DISTINCT_BYTES:
        raise SeedError('the seed does not look random')
    return seed


def load(path: Path) -> bytes:
    """The seed in PATH, refusing a file that is missing or that anyone but its owner can read."""
    try:
        status = path.stat()
    except FileNotFoundError as exc:
        raise SeedError(f'no seed at {path}: run `erista_seed.py generate`') from exc
    if stat.S_IMODE(status.st_mode) & 0o077:
        raise SeedError(f'{path} is readable by others: chmod 600')
    return parse(path.read_text())


def generate(path: Path, force: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and not force:
        raise SeedError(f'{path} exists; --force replaces it')
    flags = os.O_WRONLY | os.O_CREAT | (os.O_TRUNC if force else os.O_EXCL)
    descriptor = os.open(path, flags, 0o600)
    with os.fdopen(descriptor, 'w') as handle:
        handle.write(secrets.token_bytes(SEED_BYTES).hex() + '\n')
    path.chmod(0o600)


def fingerprint(seed: bytes) -> str:
    return hashlib.sha256(b'switch-erista-seed-fingerprint' + seed).hexdigest()[:8]


# What keeps a seeded build out of every cache. The package's actions are already no-cache and no-remote
# (boot/defs.bzl private_seed); these are the same promise from the command line, for anything that action tags miss.
CACHE_FLAGS = ('--remote_cache=', '--remote_executor=', '--noremote_accept_cached', '--noremote_upload_local_results',
               '--disk_cache=')
BUILD_FLAGS = ('--config=ios17', '--//install:prebuilt=local', '--//kernel:early_trace=true')
TARGET = '//boot:erista_first_xnu_seeded'


def command() -> str:
    return 'python3 tools/package/erista_seeded_build.py build'


def main(argv=None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    force = '--force' in args
    args = [a for a in args if a != '--force']
    if not args or args[0] not in ('generate', 'check', 'command') or len(args) > 2:
        print(__doc__, file=sys.stderr)
        return 2
    mode, path = args[0], Path(args[1]) if len(args) == 2 else DEFAULT_PATH
    try:
        if mode == 'generate':
            generate(path, force)
            print(f'seed written to {path} (fingerprint {fingerprint(load(path))}); keep it out of git and logs')
        elif mode == 'check':
            print(f'seed ok (fingerprint {fingerprint(load(path))})')
        else:
            load(path)
            print(command())
    except SeedError as exc:
        print(f'refused: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
