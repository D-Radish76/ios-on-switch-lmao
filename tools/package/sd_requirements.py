"""The exporter's conditions on the SD card (boot/storage/firmware_export/README.md), judged against `mmc info` text.

`mmc info` shows only some of what the exporter checks. `judge` marks each condition MET, NOT MET or NOT SHOWN; a
condition the screen cannot show is not treated as met. `mask` removes what identifies one card before text is kept in a
public fixture.
"""

import re

MET, NOT_MET, NOT_SHOWN = 'MET', 'NOT MET', 'NOT SHOWN'
SECTORS_LIMIT = 1 << 32
SECTOR_BYTES = 512
NOT_SHOWN_BY_MMC_INFO = (
    'single data rate (no bus mode line; Tran Speed 50 MHz agrees with it but is not a mode report)',
    'OCR ready with CCS (high capacity is its visible half)',
    'CSD version 2 (implied by SDHC or SDXC, not printed)',
    'RCA and CID nonzero',
    'CSD capacity equal to the MMC and block descriptor capacities',
    'initialized flag, controller base 0x700b0000, block device 1 on the exact drivers',
)
IDENTIFYING = ('Manufacturer ID', 'OEM', 'Name')


def _value(text: str, label: str) -> str | None:
    match = re.search(rf'^{re.escape(label)}:\s*(.+?)\s*$', text, re.MULTILINE)
    return match[1] if match else None


def capacity_bytes(text: str) -> int | None:
    match = re.search(r'^Capacity:\s*([\d.]+)\s*(GiB|MiB)', text, re.MULTILINE)
    if not match:
        return None
    return int(float(match[1]) * (1 << (30 if match[2] == 'GiB' else 20)))


def judge(info: str) -> list[tuple[str, str, str]]:
    version = re.search(r'^(\w+) version ([\d.]+)', info, re.MULTILINE)
    block = _value(info, 'Rd Block Len')
    width = _value(info, 'Bus Width')
    high = _value(info, 'High Capacity')
    size = capacity_bytes(info)
    rows = [
        ('SD (not MMC) card', _verdict(version, version and version[1] == 'SD'), version[0] if version else ''),
        ('high capacity', _verdict(high, high == 'Yes'), high or ''),
        ('512 byte blocks', _verdict(block, block == str(SECTOR_BYTES)), block or ''),
        (
            '4 bit bus (a DDR suffix would show)',
            _verdict(width, width is not None and width.startswith('4-bit') and 'DDR' not in width),
            width or '',
        ),
        (
            'at most 2^32 sectors',
            _verdict(size, size is not None and size // SECTOR_BYTES <= SECTORS_LIMIT),
            '' if size is None else str(size),
        ),
    ]
    return [*rows, *((name, NOT_SHOWN, '') for name in NOT_SHOWN_BY_MMC_INFO)]


def pages(text: str) -> dict[int, str]:
    """Split a fixture of D1-pre screens at each `END OF PAGE n` line; comment lines (#) are dropped."""
    kept = (line for line in text.splitlines() if not line.startswith('#'))
    found = {}
    current: list[str] = []
    for line in kept:
        current.append(line)
        if match := re.fullmatch(r'END OF PAGE (\d+)', line.strip()):
            found[int(match[1])] = '\n'.join(current) + '\n'
            current = []
    return found


def _verdict(present: object, ok: object) -> str:
    return NOT_SHOWN if not present else (MET if ok else NOT_MET)


def mask(info: str) -> str:
    """Replace the maker, OEM and name lines with `masked`; keep what the exporter conditions need."""
    out = []
    for line in info.splitlines():
        label = line.split(':', 1)[0]
        out.append(f'{label}: masked' if ':' in line and label in IDENTIFYING else line)
    return '\n'.join(out) + '\n'
