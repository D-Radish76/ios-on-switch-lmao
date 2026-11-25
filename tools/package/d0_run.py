"""The D0 loader's screen on the Erista (boot/diag/erista-d0-run.log), read back as numbers.

Only what the screen printed is parsed here. `decode_gic` applies the GIC-400 register layouts to the six words; it is a
reading of the values, not a check against anything the loader expects (native_gate expects no GIC values).
"""

import re

MC_ROW = re.compile(r'([0-9A-F]{8}) ([0-9A-F]{8}) (SAME|DIFFERENT|NOT READ)')
GIC_ROW = re.compile(r'GIC ([0-9A-F]{8}) ([0-9A-F]{8})')


def parse(text: str) -> dict:
    header, mc, gic = [], [], []
    for line in text.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        if match := MC_ROW.fullmatch(line):
            mc.append((int(match[1], 16), int(match[2], 16), match[3]))
        elif match := GIC_ROW.fullmatch(line):
            gic.append((int(match[1], 16), int(match[2], 16)))
        else:
            header.append(line)
    return {'header': header, 'mc': mc, 'gic': gic}


RANGE_ROW = re.compile(r'(BANK|MAP|NODE) (\d+) ([0-9A-F]{10}) ([0-9A-F]{10}) ([0-9A-F]{10})')
AREA_ROW = re.compile(r'(EXPORTER|SD REFERENCE) ([0-9A-F]{10}) ([0-9A-F]{10}) (.+)')


def ranges(lines: list[str]) -> dict[tuple[str, int], tuple[int, int, int]]:
    """D0-R's `BANK`, `MAP` and `NODE` rows as (start, size, end), keyed by kind and index."""
    return {
        (m[1], int(m[2])): (int(m[3], 16), int(m[4], 16), int(m[5], 16))
        for line in lines
        if (m := RANGE_ROW.fullmatch(line))
    }


def areas(lines: list[str]) -> dict[str, tuple[int, int, str]]:
    """D0-R's `EXPORTER` and `SD REFERENCE` rows as (start, end, verdict text as drawn)."""
    return {m[1]: (int(m[2], 16), int(m[3], 16), m[4]) for line in lines if (m := AREA_ROW.fullmatch(line))}


def field(lines: list[str], pattern: str) -> re.Match:
    for line in lines:
        if match := re.search(pattern, line):
            return match
    raise ValueError(f'no line matches {pattern!r}')


def decode_gic(words: dict[int, int]) -> dict[str, int]:
    typer, iidr = words[0x50041004], words[0x500420FC]
    return {
        'distributor_enabled': words[0x50041000] & 1,
        'interrupt_lines': 32 * ((typer & 0x1F) + 1),
        'cpu_interfaces': ((typer >> 5) & 7) + 1,
        'security_extensions': (typer >> 10) & 1,
        'sgi_ppi_enabled': words[0x50041100],
        'cpu_interface_ctlr': words[0x50042000],
        'priority_mask': words[0x50042004],
        'implementer': iidr & 0xFFF,
        'architecture': (iidr >> 16) & 0xF,
        'product': iidr >> 20,
    }
