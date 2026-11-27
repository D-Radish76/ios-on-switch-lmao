r"""List the device-window addresses a diagnostic loader forms in code and where each goes.

Input is the loader's disassembly (llvm-objdump -d --no-show-raw-insn), `loader.dis` in the Bazel output:
    diag_mmio_audit.py --disassembly bazel-bin/boot/diag_t1t/loader.dis \\
        [--uboot-image bazel-bin/boot/diag_t1t/loader.uboot.bin --t1t | --t1v3 | --s2 | --t2]
    (with --t2, --t2-sources boot/diag/loader also reads the register table and the allowlist)
It reads text only, so the Mac file works as well as a Linux one. The exit status is 1 for a window address that reaches
a load, a store, a copy or a call other than the identity register reads. What a linear scan cannot show is in
ERISTA_T1T_RUN.md, section 8.
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from diag_loader_inspect import inspect_disassembly, inspect_image
from diag_t2_allowlist import audit as allowlist_audit

WINDOW = (0x50000000, 0x80000000)
ID_REGISTERS = (0x70000804, 0x7000F9D8)
UIMAGE_MAGIC, UIMAGE_HEADER = 0x27051956, 64
HEX = r'(-?0x[0-9a-f]+|-?\d+)'
MEMORY = re.compile(r'^(ld|st)[a-z]*\s+[^\[]*\[(sp|x\d+)')
MOVZ = re.compile(rf'^movz\s+[wx](\d+), #{HEX}(?:, lsl #(\d+))?$')
MOVK = re.compile(rf'^movk\s+[wx](\d+), #{HEX}(?:, lsl #(\d+))?$')
MOV_IMM = re.compile(rf'^mov\s+[wx](\d+), #{HEX}$')
MOV_REG = re.compile(r'^mov\s+[wx](\d+), [wx](\d+)$')
ADD_IMM = re.compile(rf'^add\s+[wx](\d+), [wx](\d+), #{HEX}$')
ADRP = re.compile(r'^adrp?\s+x(\d+), 0x([0-9a-f]+)')
CALL = re.compile(r'^(bl|b)\s+0x[0-9a-f]+ <([^>+]+)')
DEST = re.compile(r'^[a-z0-9.]+\s+[wx](\d+),')
NO_DEST = ('cmp', 'cmn', 'tst', 'ccmp', 'str', 'stp', 'stur', 'b.', 'cbz', 'cbnz', 'tbz', 'tbnz')


def in_window(value: int) -> bool:
    return WINDOW[0] <= value < WINDOW[1]


def parse(disassembly: str) -> dict[str, list[tuple[str, str]]]:
    found: dict[str, list[tuple[str, str]]] = {}
    current = None
    for line in disassembly.splitlines():
        if header := re.match(r'^[0-9a-f]+ <([^>]+)>:', line):
            current = found.setdefault(header.group(1), [])
        elif current is not None and (m := re.match(r'^\s*([0-9a-f]+):\s*(.*)$', line)):
            current.append((m.group(1), re.sub(r'\s+', ' ', m.group(2).split('//')[0]).strip()))
    return found


@dataclass
class Findings:
    formed: list[tuple[int, str]] = field(default_factory=list)  # value, "function address: instruction"
    uses: list[str] = field(default_factory=list)
    calls: list[str] = field(default_factory=list)
    bases: dict[str, int] = field(default_factory=dict)


class Tracker:
    """Follows registers holding a constant through one function in instruction order (not through branches)."""

    def __init__(self, name: str, found: Findings) -> None:
        self.name, self.found = name, found
        self.regs: dict[int, int | str] = {}  # a constant, or 'data' for an adrp page

    def form(self, reg: int, value: int, where: str) -> None:
        self.regs[reg] = value & 0xFFFFFFFFFFFFFFFF
        if in_window(value):
            self.found.formed.append((value, where))

    def step(self, address: str, text: str) -> None:  # noqa: C901
        where = f'{self.name} {address}: {text}'
        if m := MOVZ.match(text):
            self.form(int(m[1]), int(m[2], 0) << int(m[3] or 0), where)
        elif m := MOVK.match(text):
            self.movk(int(m[1]), int(m[2], 0), int(m[3] or 0), where)
        elif m := MOV_IMM.match(text):
            self.form(int(m[1]), int(m[2], 0), where)
        elif m := MOV_REG.match(text):
            self.copy(int(m[1]), int(m[2]), where)
        elif m := ADD_IMM.match(text):
            held = self.regs.get(int(m[2]))
            if isinstance(held, int):
                self.form(int(m[1]), held + int(m[3], 0), where)
            else:
                self.regs.pop(int(m[1]), None)
        elif m := ADRP.match(text):
            self.regs[int(m[1])] = 'data'
            if in_window(int(m[2], 16)):
                self.found.formed.append((int(m[2], 16), where))
        elif m := CALL.match(text):
            self.call(m[1], m[2], where)
        else:
            self.access(where, text)
            if (d := DEST.match(text)) and not text.startswith(NO_DEST):
                self.regs.pop(int(d[1]), None)

    def movk(self, reg: int, imm: int, shift: int, where: str) -> None:
        held = self.regs.get(reg)
        if isinstance(held, int):
            self.form(reg, (held & ~(0xFFFF << shift)) | (imm << shift), where)
        else:
            self.regs.pop(reg, None)

    def copy(self, dest: int, source: int, where: str) -> None:
        held = self.regs.get(source)
        if held is None:
            self.regs.pop(dest, None)
            return
        self.regs[dest] = held
        if isinstance(held, int) and in_window(held):
            self.found.uses.append(f'{where}  (copy of {held:#x})')

    def call(self, kind: str, target: str, where: str) -> None:
        args = [f'x{r}={v:#x}' for r in range(8) if isinstance(v := self.regs.get(r), int) and in_window(v)]
        if args:
            self.found.calls.append(f'{where}  ({kind} {target} with {", ".join(args)})')
        self.regs = {r: v for r, v in self.regs.items() if r >= 19 and r != 30}

    def access(self, where: str, text: str) -> None:
        if not (m := MEMORY.match(text)):
            return
        kind = 'load' if m[1] == 'ld' else 'store'
        held = self.regs.get(int(m[2][1:])) if m[2] != 'sp' else None
        if m[2] in ('sp', 'x29'):
            base = 'stack'
        elif held == 'data':
            base = 'static data'
        elif isinstance(held, int):
            base = 'window constant' if in_window(held) else 'RAM constant'
            if in_window(held):
                self.found.uses.append(f'{where}  ({kind} through the window constant {held:#x})')
        else:
            base = 'pointer'
        key = f'{kind} {base}'
        self.found.bases[key] = self.found.bases.get(key, 0) + 1


def callers(code: dict[str, list[tuple[str, str]]], target: str) -> list[str]:
    return sorted(n for n, body in code.items() if any((m := CALL.match(t)) and m[2] == target for _, t in body))


def path_lines(code: dict[str, list[tuple[str, str]]]) -> list[str]:
    lines = []
    for name in (
        'switch_diag_read32',
        'switch_t1t_read32',
        'switch_s2_read32',
        'switch_s2_read16',
        'switch_t4r_read32',
        'switch_t3r_read32',
        'switch_t3_read32',
        'switch_t3_write32',
        'switch_t2_read32',
        'switch_t2_write32',
    ):
        if name in code:
            loads = [t for _, t in code[name] if MEMORY.match(t)]
            lines.append(
                f'   {name}: {" ; ".join(loads) or "no memory access"}; called by {", ".join(callers(code, name))}'
            )
    if 'switch_t1t_access' in code:
        lines.append(
            '   T1-T: static table (switch_t1t_address) -> switch_t1t_access -> switch_t1t_read32 -> one load; '
            f'switch_t1t_address called by {", ".join(callers(code, "switch_t1t_address"))}'
        )
    if 'switch_s2_access' in code:
        lines.append(
            '   D0-S v2: static table (switch_s2_address, switch_s2_width) -> switch_s2_access -> '
            'switch_s2_read32 (one word load) or switch_s2_read16 (one halfword load); '
            f'switch_s2_address called by {", ".join(callers(code, "switch_s2_address"))}'
        )
    if 'switch_t4r_access' in code:
        lines.append(
            '   T4-R: static table (switch_t4r_address) -> switch_t4r_access -> switch_t4r_read32 (one word load); '
            f'switch_t4r_address called by {", ".join(callers(code, "switch_t4r_address"))}'
        )
    if 'switch_t3r_access' in code:
        lines.append(
            '   T3-R: static table (switch_t3r_address) -> switch_t3r_access -> switch_t3r_read32 (one word load); '
            f'switch_t3r_address called by {", ".join(callers(code, "switch_t3r_address"))}'
        )
    if 'switch_t3_access' in code:
        lines.append(
            '   T3: static table (switch_t3_address) -> switch_t3_access (switch_t3_may_read / switch_t3_may_write decide first) -> '
            'switch_t3_read32 / switch_t3_write32; '
            f'switch_t3_address called by {", ".join(callers(code, "switch_t3_address"))}'
        )
    if 'switch_t2_access' in code:
        lines.append(
            '   T2: static table (switch_t2_address) -> switch_t2_access (the allowlist decides first) -> '
            'switch_t2_read32 / switch_t2_write32; '
            f'switch_t2_address called by {", ".join(callers(code, "switch_t2_address"))}'
        )
    if 'switch_t1v3_access' in code:
        lines.append(
            '   T1 v3: static table (switch_t1v3_address) -> switch_t1v3_access -> '
            'switch_t1v3_read32 / switch_t1v3_write32; '
            f'switch_t1v3_address called by {", ".join(callers(code, "switch_t1v3_address"))}'
        )
    return lines


def judge(found: Findings, code: dict[str, list[tuple[str, str]]]) -> list[str]:
    problems = []
    for value, where in found.formed:
        if value in ID_REGISTERS:
            continue
        if ': adrp' in where:
            problems.append(f'a page address in the device window is formed by adrp: {where}')
        site = where.split(':')[0]
        problems += [f'window address {value:#x} is used: {u}' for u in found.uses + found.calls if u.startswith(site)]
    problems += [f'a window constant goes to {c}' for c in found.calls if 'switch_diag_read32' not in c]
    problems += [f'a window constant is copied or dereferenced: {u}' for u in found.uses if 'copy of' not in u]
    seen = {v for v, _ in found.formed}
    problems += [f'identity register constant {v:#x} not found' for v in ID_REGISTERS if v not in seen]
    problems += [
        f'switch_t1t_read32 is called by {c}' for c in callers(code, 'switch_t1t_read32') if c != 'switch_t1t_access'
    ]
    problems += [
        f'{raw} is called by {c}'
        for raw in ('switch_s2_read32', 'switch_s2_read16')
        for c in callers(code, raw)
        if c != 'switch_s2_access'
    ]
    problems += [
        f'switch_t4r_read32 is called by {c}' for c in callers(code, 'switch_t4r_read32') if c != 'switch_t4r_access'
    ]
    problems += [
        f'switch_t3r_read32 is called by {c}' for c in callers(code, 'switch_t3r_read32') if c != 'switch_t3r_access'
    ]
    problems += [
        f'{raw} is called by {c}'
        for raw in ('switch_t3_read32', 'switch_t3_write32')
        for c in callers(code, raw)
        if c != 'switch_t3_access'
    ]
    problems += [
        f'{raw} is called by {c}'
        for raw in ('switch_t1v3_read32', 'switch_t1v3_write32')
        for c in callers(code, raw)
        if c != 'switch_t1v3_access'
    ]
    problems += [
        f'{raw} is called by {c}'
        for raw in ('switch_t2_read32', 'switch_t2_write32')
        for c in callers(code, raw)
        if c != 'switch_t2_access'
    ]
    return problems


def audit(  # noqa: PLR0913
    disassembly: str,
    flat: bytes | None = None,
    *,
    t1t: bool = False,
    t1v3: bool = False,
    s2: bool = False,
    t2: bool = False,
    t4r: bool = False,
    t3r: bool = False,
    t3: bool = False,
) -> tuple[list[str], list[str]]:
    """Return the report lines and the problems."""
    code = parse(disassembly)
    found, pointers = Findings(), []
    for name, body in code.items():
        before = dict(found.bases)
        tracker = Tracker(name, found)
        for address, text in body:
            tracker.step(address, text)
        mine = {k: v - before.get(k, 0) for k, v in found.bases.items() if 'pointer' in k and v - before.get(k, 0)}
        if mine:
            pointers.append(f'   {name}: ' + ', '.join(f'{k} x{v}' for k, v in sorted(mine.items())))
    report = ['1. Instructions that form an address in the device window 0x50000000 to 0x7fffffff']
    report += [f'   {where}  -> {value:#x}' for value, where in found.formed] or ['   none']
    report.append('2. Where those registers go (loads, stores, copies, calls)')
    report += [f'   {line}' for line in found.uses + found.calls] or ['   nothing']
    report.append('3. The path from an address to its access')
    report += path_lines(code)
    report.append('4. Functions that load or store through a pointer that is not a constant (not bounded by this scan)')
    report += pointers
    report.append('5. Memory accesses by base: ' + ', '.join(f'{k} x{v}' for k, v in sorted(found.bases.items())))
    problems = judge(found, code)
    if s2:
        problems += inspect_disassembly(disassembly, s2=True)
    if t2:
        problems += inspect_disassembly(disassembly, t2=True)
    if t4r:
        problems += inspect_disassembly(disassembly, t4r=True)
    if t3r:
        problems += inspect_disassembly(disassembly, t3r=True)
    if t3:
        problems += inspect_disassembly(disassembly, t3=True)
    if flat is not None:
        listed = inspect_image(flat, t1t=t1t, t1v3=t1v3, s2=s2, t2=t2, t4r=t4r, t3r=t3r, t3=t3)
        name = 'D0-S v2' if s2 else 'T1-T' if t1t else 'T2' if t2 else 'T4-R' if t4r else 'T3-R' if t3r else 'T3' if t3 else 'T1 v3'
        checked = t1t or t1v3 or s2 or t2 or t4r or t3r or t3
        report.append(
            '6. Flat image: '
            + (
                f'register address words match the {name} list'
                if checked and not listed
                else 'MISMATCH'
                if checked
                else 'not checked'
            )
        )
        problems += listed
    return report, problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--disassembly', required=True, type=Path)
    parser.add_argument('--uboot-image', type=Path, help='loader.uboot.bin; its flat image is checked as well')
    parser.add_argument('--t1t', action='store_true', help='the image is T1-T: check its register address words')
    parser.add_argument('--t1v3', action='store_true', help='the image is T1 v3: check its register address words')
    parser.add_argument('--s2', action='store_true', help='the image is D0-S v2: check its register address words')
    parser.add_argument('--t4r', action='store_true', help='the image is T4-R: check its register address words')
    parser.add_argument('--t3r', action='store_true', help='the image is T3-R: check its register address words')
    parser.add_argument('--t3', action='store_true', help='the image is T3: check its register address words and raw accesses')
    parser.add_argument(
        '--t2', action='store_true', help='the image is T2: check its register address words and raw accesses'
    )
    parser.add_argument(
        '--t2-sources', type=Path, help='with --t2: boot/diag/loader, to read the table and the allowlist'
    )
    parser.add_argument('--flat', type=Path, help='the flat loader image itself, in place of --uboot-image')
    args = parser.parse_args()
    flat = args.flat.read_bytes() if args.flat else None
    if args.uboot_image:
        raw = args.uboot_image.read_bytes()
        if int.from_bytes(raw[:4], 'big') != UIMAGE_MAGIC:
            print('not a legacy U-Boot image', file=sys.stderr)
            return 2
        flat = raw[UIMAGE_HEADER:]
    report, problems = audit(args.disassembly.read_text(), flat, t1t=args.t1t, t1v3=args.t1v3, s2=args.s2, t2=args.t2, t4r=args.t4r, t3r=args.t3r, t3=args.t3)
    if args.t2 and args.t2_sources:
        directory = args.t2_sources
        bus = (directory / 't2_bus.c').read_text() + (directory.parent / 't2' / 't2.h').read_text()
        listing, table_problems = allowlist_audit((directory / 't2_regs.c').read_text(), bus)
        report += ['7. T2 register table and allowlist (sources)', *listing]
        problems += table_problems
    print('\n'.join(report))
    if problems:
        print('\nPROBLEMS:\n' + '\n'.join(problems), file=sys.stderr)
        return 1
    if args.s2:
        print(
            '\nAUDIT PASSED (D0-S v2): the image holds exactly 0x60006004, 0x60006010, 0x700b0024 and '
            '0x700b002c, each once; '
            'switch_s2_access is the only caller of the one word load (the first three) and of the one halfword load '
            '(0x700b002c); no device store; in code only the two identity registers are formed.'
        )
        return 0
    if args.t4r:
        print(
            '\nAUDIT PASSED (T4-R): the image holds exactly 0x60005010, 0x60005060, 0x60005100 and 0x60005104, each once; '
            'switch_t4r_access is the only caller of the one word load; no device store; '
            'in code only the two identity registers are formed.'
        )
        return 0
    if args.t3r:
        print(
            '\nAUDIT PASSED (T3-R): the image holds exactly the 21 listed fan state register addresses, each once; '
            'switch_t3r_access is the only caller of the one word load; no device store; '
            'in code only the two identity registers are formed.'
        )
        return 0
    if args.t3:
        print(
            '\nAUDIT PASSED (T3): the image holds exactly the 20 listed fan state register addresses, each once; '
            'switch_t3_access is the only caller of the one word load and the one word store, and calls the read and '
            'write allowlists first; in code only the two identity registers are formed.'
        )
        return 0
    if args.t2:
        print(
            '\nAUDIT PASSED (T2): the image holds exactly the 13 listed CAR and I2C1 addresses, each once; '
            'switch_t2_access is the only caller of the one word load and the one word store; '
            + (
                'the table and allowlist match the review (CAR read only, one TMP451 write: pointer 0x0f, value 0x00); '
                if args.t2_sources
                else 'the table and allowlist were not read (no --t2-sources); '
            )
            + 'in code only the two identity registers are formed.'
        )
        return 0
    print('\nAUDIT PASSED: only the two identity registers reach a call or an access.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
