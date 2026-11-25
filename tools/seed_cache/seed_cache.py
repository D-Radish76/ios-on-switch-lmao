#!/usr/bin/env python3
"""Put a download Bazel keeps timing out on into its repository cache, checked against the pinned sha256.

    seed_cache.py ipsw --pins ipsw/pins.bzl --kind 21H461 --ipsw FILE [--cache DIR]
    seed_cache.py file --sha256 HEX FILE [--cache DIR]

`ipsw` cuts every pinned member of the kind out of a whole IPSW (fetched with `curl -C -`, which resumes). `file` stores
one archive whole, such as the LLVM release. A member or file whose bytes do not match its sha256 is not stored.
Bazel finds each entry by that sha256, so the repository rules that download it need no network.
"""

import argparse
import hashlib
import os
import sys
import tempfile
from pathlib import Path

CHUNK = 8 << 20
DEFAULT_CACHE = '~/Library/Caches/switch-ios-bazel/repository'  # --repository_cache in .bazelrc


def store(source: Path, offset: int, size: int, sha256: str, cache: Path) -> bool:
    """Copy source[offset:offset+size] to the cache entry for sha256, 8 MiB at a time; False if the bytes differ."""
    entry = cache / 'content_addressable' / 'sha256' / sha256
    entry.mkdir(parents=True, exist_ok=True)
    handle, part = tempfile.mkstemp(dir=entry, prefix='.part')
    digest, left = hashlib.sha256(), size
    try:
        with source.open('rb') as src, os.fdopen(handle, 'wb') as out:
            src.seek(offset)
            while left:
                block = src.read(min(CHUNK, left))
                if not block:
                    break
                digest.update(block)
                out.write(block)
                left -= len(block)
        if left or digest.hexdigest() != sha256:
            return False
        os.replace(part, entry / 'file')
        return True
    finally:
        if os.path.exists(part):
            os.unlink(part)


def pinned_members(pins: Path, kind: str) -> dict:
    scope: dict = {}
    exec(pins.read_text(), scope)
    return scope['IPSWS'][kind]['members']


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    step = parser.add_subparsers(dest='step', required=True)
    ipsw = step.add_parser('ipsw')
    ipsw.add_argument('--pins', type=Path, required=True)
    ipsw.add_argument('--kind', required=True)
    ipsw.add_argument('--ipsw', type=Path, required=True)
    file = step.add_parser('file')
    file.add_argument('--sha256', required=True)
    file.add_argument('file', type=Path)
    for p in (ipsw, file):
        p.add_argument('--cache', type=Path, default=Path(DEFAULT_CACHE).expanduser())
    args = parser.parse_args()

    if args.step == 'file':
        jobs = {args.file.name: (0, args.file.stat().st_size, args.sha256)}
        source = args.file
    else:
        members = pinned_members(args.pins, args.kind)
        jobs = {role: (m['offset'], m['stored_size'], m['sha256']) for role, m in sorted(members.items())}
        source = args.ipsw
    failed = 0
    for name, (offset, size, sha256) in jobs.items():
        ok = store(source, offset, size, sha256, args.cache)
        failed += not ok
        print(('stored  ' if ok else 'MISMATCH, not stored  ') + name, f'{size} bytes')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
