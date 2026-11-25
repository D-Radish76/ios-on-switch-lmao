"""What the D1 diagnostic requires of the pinned exporter and of the two U-Boot cache helpers before it may run them.

`check_exporter` refuses an image that is not the pinned file or whose header disagrees with where `loader_abi` runs it.
The script then repeats the same facts at run time from the constants returned here, never from literals of its own:
every header word of the image in its slot, its data CRC32 in the slot and again at the run address, and the three
instructions of each helper. CRC32 detects corruption; it is not a defence against tampering.
"""

import hashlib
import struct
import zlib
from typing import NamedTuple

from loader_abi import BL33_BASE, EXPORTER_RUN, EXPORTER_SLOT

HEADER_BYTES = 64
HEADER_WORDS = HEADER_BYTES // 4
HELPERS = (0xAA001920, 0xAA001A64)
HELPER_WORDS = 3
MAGIC, OS_STANDALONE, ARCH_AARCH64, TYPE_STANDALONE, NO_COMPRESSION = 0x27051956, 17, 22, 1, 0
NAME = 'Switch MMC RAM Export v1'
EXPORTER_PIN = 'firmware/exporter.uimg'
BL33_PIN = 'firmware/bl33.bin'


class Exporter(NamedTuple):
    data_bytes: int
    data_crc: int
    header_words: tuple[int, ...]


def pin(sums: str, path: str) -> str:
    rows = [line.split() for line in sums.splitlines() if line.split()[-1:] == [path]]
    if len(rows) != 1:
        raise SystemExit(f'SHA256SUMS lists {len(rows)} lines for {path}')
    return rows[0][0]


def check_pinned(data: bytes, sums: str, path: str) -> None:
    if hashlib.sha256(data).hexdigest() != pin(sums, path):
        raise SystemExit(f'{path} is not the pinned file')


def check_exporter(image: bytes, sums: str) -> Exporter:
    check_pinned(image, sums, EXPORTER_PIN)
    magic, hcrc, _, size, load, entry, dcrc, os_type, arch, kind, comp, name = struct.unpack(
        '>7I4B32s', image[:HEADER_BYTES]
    )
    header = image[:4] + bytes(4) + image[8:HEADER_BYTES]
    problems = [
        text
        for text, bad in (
            ('magic', magic != MAGIC),
            ('header CRC', hcrc != zlib.crc32(header)),
            (
                'OS, architecture, type or compression',
                (os_type, arch, kind, comp) != (OS_STANDALONE, ARCH_AARCH64, TYPE_STANDALONE, NO_COMPRESSION),
            ),
            ('load or entry address', load != EXPORTER_RUN[0] or entry != EXPORTER_RUN[0]),
            ('name', name.rstrip(b'\0') != NAME.encode()),
            ('length', HEADER_BYTES + size != len(image)),
            ('data CRC', dcrc != zlib.crc32(image[HEADER_BYTES:])),
            ('size against the run area', size > EXPORTER_RUN[1]),
            ('size against the slot', len(image) > EXPORTER_SLOT[1]),
        )
        if bad
    ]
    if problems:
        raise SystemExit('exporter.uimg header: ' + ', '.join(problems))
    return Exporter(size, dcrc, struct.unpack('<16I', image[:HEADER_BYTES]))


def helper_words(bl33: bytes, address: int) -> tuple[int, ...]:
    offset = address - BL33_BASE
    return struct.unpack(f'<{HELPER_WORDS}I', bl33[offset : offset + 4 * HELPER_WORDS])


def word_tests(address: int, words: tuple[int, ...]) -> str:
    return ' && '.join(f'itest.l *{address + 4 * i:#x} == {word:#x}' for i, word in enumerate(words))


def crc_as_stored(crc: int) -> int:
    """Return the word `itest.l` reads after `crc32 ... dest`, which stores the CRC big endian."""
    return struct.unpack('<I', struct.pack('>I', crc))[0]


# The status block the script fills in and the loader reads (boot/diag/sd_loader/sd_report.h). It sits in the exporter's
# 64 KiB run area, above the image and its stack (the exporter writes only 0xA7000000..0xA7005E50); D0-R found the
# area clear.
STATUS_BASE = EXPORTER_RUN[0] + 0xF000
STATUS_MAGIC = 0x31445353
STATUS_VERSION = 1
STATUS_BYTES = 0x100
STATUS_OFFSETS = {
    'MAGIC': 0x00,
    'VERSION': 0x04,
    'EXPECTED_CRC': 0x08,
    'PAYLOAD_BYTES': 0x0C,
    'CRC_SLOT': 0x10,
    'CRC_LOADED': 0x14,
    'CRC_BEFORE_GO': 0x18,
}
STATUS_FLAGS = (
    'HEADER',
    'SLOT_CRC',
    'HELPERS',
    'LOADED',
    'LOADED_CRC',
    'READ',
    'BEFORE_GO_CRC',
    'GO_RC0',
    'RECORD',
    'REFERENCE',
)
FLAGS_OFFSET = 0x20


def exporter_values(exporter: Exporter, bl33: bytes) -> dict[str, str]:
    """Return every `@NAME@` the D1 script template takes beyond D0's, from the pinned files only."""
    values = {
        'EXPORTER': hex(EXPORTER_SLOT[0]),
        'EXPORTER_DATA': hex(EXPORTER_SLOT[0] + HEADER_BYTES),
        'EXPORTER_RUN': hex(EXPORTER_RUN[0]),
        'EXPORTER_BYTES': hex(exporter.data_bytes),
        'EXPECTED_CRC_STORED': hex(crc_as_stored(exporter.data_crc)),
        'EXPORTER_HEADER_TESTS': word_tests(EXPORTER_SLOT[0], exporter.header_words),
        'STATUS': hex(STATUS_BASE),
        'STATUS_BYTES': hex(STATUS_BYTES),
        'STATUS_MAGIC': hex(STATUS_MAGIC),
        'STATUS_VERSION': str(STATUS_VERSION),
    }
    for index, address in enumerate(HELPERS):
        values[f'HELPER_{index}'] = hex(address)
        values[f'HELPER_TESTS_{index}'] = word_tests(address, helper_words(bl33, address))
    values.update({f'S_{name}': hex(STATUS_BASE + offset) for name, offset in STATUS_OFFSETS.items()})
    values.update({f'S_{name}': hex(STATUS_BASE + FLAGS_OFFSET + 4 * i) for i, name in enumerate(STATUS_FLAGS)})
    return values
