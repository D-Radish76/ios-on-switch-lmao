"""Where the seeded build leaves things: the outputs that hold the seed by design, and the places that must not."""

import re
import subprocess
from pathlib import Path

OUTPUTS = ('erista_first_xnu_seeded_work', 'erista-first-xnu-seeded', 'erista-full-ram-seeded.adt')
CONTROL = 'erista-full-ram-seeded.adt'
# Under bazel's output base. The seeded outputs are in execroot by design; the rest of it must not hold the seed.
EXCLUDED = {
    'execroot': 'holds the seeded outputs by design',
    'external': 'downloaded sources; nothing there is derived from the seed',
    'install': 'the bazel installation',
}
DISK_CACHE = re.compile(r'--disk_cache=(\S+)')


def bazel_info(key: str, run=subprocess.run) -> Path:
    done = run(['bazel', 'info', key], capture_output=True, text=True, check=False)
    if done.returncode != 0 or not done.stdout.strip():
        raise OSError(f'bazel info {key} failed')
    return Path(done.stdout.strip())


def disk_caches(rc_files: list[Path]) -> list[Path]:
    """Every non-empty --disk_cache in the rc files, in any config: a config may be switched on without this tool seeing it."""
    found = []
    for file in rc_files:
        if not file.is_file():
            continue
        for line in file.read_text(errors='replace').splitlines():
            if line.lstrip().startswith('#'):
                continue
            for value in DISK_CACHE.findall(line):
                path = Path(value.strip('"\'')).expanduser()
                if value.strip('"\'') and path not in found:
                    found.append(path)
    return found


def output_base_places(base: Path) -> tuple[list[Path], list[str]]:
    places, notes = [], []
    for entry in sorted(base.iterdir()):
        if entry.name in EXCLUDED:
            notes.append(f'not scanned: {entry} ({EXCLUDED[entry.name]})')
        else:
            places.append(entry)
    return places, notes


def seeded_outputs(bazel_bin: Path) -> list[Path]:
    return [bazel_bin / 'boot' / name for name in OUTPUTS if (bazel_bin / 'boot' / name).exists()]
