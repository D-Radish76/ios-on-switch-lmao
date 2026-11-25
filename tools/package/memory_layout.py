"""The Erista's DRAM layout from the D0-pre run, set against where the D0 loader and the package put things.

`parse_bdinfo` reads boot/diag/erista-memory-bdinfo.log (U-Boot's bdinfo as reported). `regions` lists what is known to
occupy memory, each tagged with how it is known; `overlaps` is every pair of the D0 loader's span and a region that
touches. Nothing here proves a range free: only the regions listed are compared.
"""

import re
from dataclasses import dataclass

from loader_abi import BL33_BASE, BOOT_ARGS_PAGE, NATIVE_EXCLUSIONS, SD_REFERENCE

MEASURED, DECODED, SOURCE, INFERRED = 'measured', 'decoded', 'source', 'inferred'
MIB = 1 << 20
MC_CARVEOUTS = (
    ('GPU firmware (MC carveout 2)', 0x70019C5C, 0x70019C64),
    ('GPU WPR (MC carveout 3)', 0x70019CAC, 0x70019CB4),
    ('secure firmware (MC carveout 1)', 0x70019C0C, 0x70019C14),
)
TZDRAM_BOM = 0x700199A0
CARVEOUT_UNIT = 128 << 10
HEKATE_DTB = 0xA8000000
HEKATE_ENV_BYTES = 256 << 10
LOADER_SPAN = (0xA0000000, 0xA00151D0)
OWN = ('adapter-stack-vectors', 'bl33-and-kernel-window')


@dataclass(frozen=True)
class Region:
    name: str
    start: int
    end: int
    how: str


def parse_bdinfo(text: str) -> dict[str, int | list[tuple[int, int]]]:
    values: dict[str, int] = {}
    banks: dict[int, dict[str, int]] = {}
    for line in text.splitlines():
        if line.startswith('#') or '=' not in line:
            continue
        key, raw = (part.strip() for part in line.split('=', 1))
        bank = re.fullmatch(r'DRAM bank (\d+) (start|size)', key)
        number = re.match(r'(0x[0-9a-fA-F]+|\d+)', raw)
        if not number:
            raise ValueError(f'no number in {line!r}')
        if bank:
            banks.setdefault(int(bank.group(1)), {})[bank.group(2)] = int(number.group(1), 0)
        else:
            values[key] = int(number.group(1), 0)
    if sorted(banks) != list(range(len(banks))) or any(set(b) != {'start', 'size'} for b in banks.values()):
        raise ValueError('the DRAM banks are not numbered from 0 with a start and a size each')
    return {**values, 'banks': [(banks[i]['start'], banks[i]['size']) for i in sorted(banks)]}


def mc_registers(text: str) -> dict[int, int]:
    return {
        int(match.group(1), 16): int(match.group(2), 16)
        for match in re.finditer(r'^([0-9a-f]{8}): ([0-9a-f]{8})$', text, re.MULTILINE)
    }


def carveouts(registers: dict[int, int]) -> list[Region]:
    found = [
        Region(name, registers[bom], registers[bom] + registers[size] * CARVEOUT_UNIT, DECODED)
        for name, bom, size in MC_CARVEOUTS
    ]
    base = registers[TZDRAM_BOM]
    return [*found, Region('TZDRAM, BL31 (hekate TZDRAM_BASE)', base, base + MIB, INFERRED)]


def regions(bdinfo: dict, registers: dict[int, int]) -> list[Region]:
    top = bdinfo['TLB addr'] + 0x10000
    return [
        *carveouts(registers),
        Region('U-Boot TLB (size assumed 64 KiB)', bdinfo['TLB addr'], top, INFERRED),
        Region('U-Boot relocated image', bdinfo['relocaddr'], bdinfo['TLB addr'], MEASURED),
        Region('U-Boot FDT, stack top', bdinfo['sp start'], bdinfo['fdt_blob'] + 0x1000, INFERRED),
        Region('U-Boot as loaded by hekate and the staged window', BL33_BASE, 0xB0000000, SOURCE),
        Region('hekate BL33 environment', BL33_BASE - HEKATE_ENV_BYTES, BL33_BASE, SOURCE),
        Region(
            'hekate DTB (size unknown, ends before the environment)', HEKATE_DTB, BL33_BASE - HEKATE_ENV_BYTES, SOURCE
        ),
        Region('boot arguments page', BOOT_ARGS_PAGE, BOOT_ARGS_PAGE + 0x1000, SOURCE),
        Region('SD reference page', SD_REFERENCE, SD_REFERENCE + 0x1000, SOURCE),
        *(Region(name, start, start + size, SOURCE) for name, start, size in NATIVE_EXCLUSIONS if name not in OWN),
    ]


def overlaps(span: tuple[int, int], listed: list[Region]) -> list[Region]:
    return [r for r in listed if span[0] < r.end and r.start < span[1]]


def banks_end(bdinfo: dict) -> int:
    return max(start + size for start, size in bdinfo['banks'])
