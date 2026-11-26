"""Looks for the per-install seed in files and directory trees, without ever printing it.

Places come in two kinds. A *leak* place must not hold the seed (git's files, a disk cache, logs). An *expected* place
holds it by design (the seeded outputs, the SD card copy); a hit there is counted and is not a leak.
"""

import os
import stat
from dataclasses import dataclass, field
from pathlib import Path

CHUNK = 1 << 20

NOT_DETECTED = (
    'a byte scan does not see: compressed or encrypted data (an LZ4 or zstd block, a gzip or zip file, a compressed '
    'cache blob), the seed split across files or in another encoding (base64, UTF-16), anything derived from it, '
    'and every place nobody listed'
)


class ScanError(Exception):
    pass


@dataclass
class Report:
    scanned: int = 0
    leaks: list[Path] = field(default_factory=list)
    expected_hits: list[Path] = field(default_factory=list)
    expected_scanned: int = 0
    unreadable: list[Path] = field(default_factory=list)
    links: list[Path] = field(default_factory=list)
    special: list[Path] = field(default_factory=list)
    places: int = 0

    @property
    def clean(self) -> bool:
        return not self.leaks and not self.unreadable


def needles(seed: bytes) -> tuple[bytes, ...]:
    """The seed's middle 64 bytes, which the pinned seed does not share: raw, and as lower and upper case hex."""
    middle = seed[96:160]
    return middle, middle.hex().encode(), middle.hex().upper().encode()


def holds(path: Path, wanted: tuple[bytes, ...]) -> bool:
    """Whether the file holds a needle, read in chunks that overlap by the longest needle."""
    overlap = max(map(len, wanted)) - 1
    tail = b''
    with path.open('rb') as handle:
        while block := handle.read(CHUNK):
            window = tail + block
            if any(n in window for n in wanted):
                return True
            tail = window[-overlap:]
    return False


def walk(root: Path, report: Report):
    """Regular files under root. A link is never followed (so a cycle cannot loop), and is reported."""
    seen = set()
    for directory, names, files in os.walk(
        root, followlinks=False, onerror=lambda e: report.unreadable.append(Path(e.filename))
    ):
        here = Path(directory)
        info = os.stat(here)
        if (info.st_dev, info.st_ino) in seen:
            names[:] = []
            continue
        seen.add((info.st_dev, info.st_ino))
        for name in list(names):
            if (here / name).is_symlink():
                report.links.append(here / name)
                names.remove(name)
        for name in files:
            yield here / name


def visit(path: Path, wanted: tuple[bytes, ...], report: Report, expected: bool) -> None:
    if path.is_symlink():
        report.links.append(path)
        return
    try:
        mode = os.lstat(path).st_mode
    except OSError:
        report.unreadable.append(path)
        return
    if not stat.S_ISREG(mode):
        report.special.append(path)
        return
    try:
        found = holds(path, wanted)
    except OSError:
        report.unreadable.append(path)
        return
    if expected:
        report.expected_scanned += 1
        if found:
            report.expected_hits.append(path)
    else:
        report.scanned += 1
        if found:
            report.leaks.append(path)


def scan(seed: bytes, leak_places: list[Path], expected_places: list[Path] = ()) -> Report:
    """A missing place is an error; an unreadable file is reported and makes the report not clean."""
    wanted = needles(seed)
    report = Report()
    for expected, places in ((False, leak_places), (True, expected_places)):
        for place in places:
            if not os.path.lexists(place):
                raise ScanError(f'{place} does not exist')
            report.places += 1
            if place.is_symlink() and not place.is_dir():
                visit(place, wanted, report, expected)
            elif place.is_dir():
                for path in walk(place.resolve() if place.is_symlink() else place, report):
                    visit(path, wanted, report, expected)
            else:
                visit(place, wanted, report, expected)
    return report


def summary(report: Report) -> list[str]:
    lines = [
        f'scanned {report.scanned} files in {report.places} places for leaks, {report.expected_scanned} in expected places',
        f'not followed: {len(report.links)} symbolic links, {len(report.special)} special files; unreadable: {len(report.unreadable)}',
    ]
    if report.expected_scanned:
        lines.append(
            f'expected places holding the seed: {len(report.expected_hits)} of {report.expected_scanned} files'
        )
    lines.append(f'not detected: {NOT_DETECTED}')
    return lines
