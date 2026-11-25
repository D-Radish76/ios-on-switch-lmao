#!/usr/bin/env python3
"""What the D0 loader's binary may contain, read from its link map and its disassembly.

    diag_loader_inspect.py --map MAP --disassembly DIS [--report OUT]

Exits nonzero, naming every violation, when the link pulled in the normal loader's kernel path, when code writes a CPU
system register that WRITES does not list, or when an instruction outside the reviewed set is present. WRITES is the
review's list: every change to CPU state the diagnostic makes, and why.
"""

import argparse
import re
import sys
from pathlib import Path

# llvm-objdump's spelling of each msr target, what the write is for and how often it appears. entry.S is the normal
# loader's EL2 -> EL1 sequence.
WRITES = {
    'DAIFSet': ('mask D, A, I and F before anything runs', 3),
    'SPSel': ('use SP_ELx, not SP_EL0, for the private stack', 2),
    'VBAR_EL2': ('exception vectors at EL2, so a fault is reported', 1),
    'CNTHCTL_EL2': ('EL1 may read the counter and timer (3)', 1),
    'CNTVOFF_EL2': ('virtual counter offset 0', 1),
    'VPIDR_EL2': ('EL1 reads the real MIDR', 1),
    'VMPIDR_EL2': ('EL1 reads the real MPIDR', 1),
    'CPTR_EL2': ('no EL2 trap of EL1 FP/SIMD or trace', 1),
    'HSTR_EL2': ('no trap of EL1 CP15 accesses', 1),
    'CPACR_EL1': ('0: FP/SIMD stays off, the code uses integer registers only', 1),
    'SCTLR_EL1': ('EL1 starts with MMU, caches and alignment checks off', 1),
    'TTBR0_EL1': ('0: no translation tables, MMU stays off', 1),
    'TTBR1_EL1': ('0: no translation tables, MMU stays off', 1),
    'TCR_EL1': ('0: reset value', 1),
    'MAIR_EL1': ('0: reset value', 1),
    'CONTEXTIDR_EL1': ('0: reset value', 1),
    'CNTKCTL_EL1': ('0 at EL1 entry; then the event stream (EVNTEN, EVNTI 15) so the 15 s wait sleeps in wfe', 2),
    'SP_EL1': ('the EL1 stack, the same private stack', 1),
    'VBAR_EL1': ('exception vectors at EL1', 1),
    'HCR_EL2': ('RW only (EL1 is AArch64): no stage 2, no traps, no interrupt routing', 1),
    'SPSR_EL2': ('0x3c5: EL1h with D, A, I and F masked, for the eret', 1),
    'ELR_EL2': ('where the eret lands: the EL1 half of the entry', 1),
}
# Instructions other than loads, stores, arithmetic and branches that may appear, with how many times.
SPECIAL = {'eret': 1, 'smc': 1, 'tlbi': 1, 'wfe': 2}
FORBIDDEN_MNEMONICS = {
    'hvc',
    'svc',
    'ic',
    'dc',
    'at',
    'sys',
    'sysl',
    'brk',
    'hlt',
    'dcps1',
    'dcps2',
    'dcps3',
    'wfi',
    'sev',
    'sevl',
}
FORBIDDEN_SYMBOLS = (
    'loader_enter_kernel',
    'loader_blocks',
    'loader_copy',
    'loader_emc',
    'loader_seed',
    'loader_boot_args',
    'switch_sd_',
    'lz4_block',
    'exporter',
    'copy_tables',
    'kernel_window',
)
REQUIRED_SYMBOLS = ('switch_diag_read32', 'switch_diag_exit', 'switch_diag_after_transition', 'probe_show_exception')
# The one place a device register address is formed. It loads and returns.
READ32_BODY = re.compile(r'\A(?:mov\s+w\d+, w0\s*\n)?ldr\s+w0, \[x\d+\]\s*\nret\s*\Z')
# Device addresses built as an immediate in code: 0x5000xxxx to 0x7fffxxxx, except the identity registers (0x7000....).
DEVICE_IMMEDIATE = re.compile(r'\b(?:movk|movz|mov)\s+[wx]\d+, #0x([0-9a-f]{4}), lsl #16')


def inspect_map(text: str) -> list[str]:
    problems = [f'the link has {name}' for name in FORBIDDEN_SYMBOLS if name in text]
    problems += [f'the link lacks {name}' for name in REQUIRED_SYMBOLS if name not in text]
    return problems


def functions(disassembly: str) -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}
    current = None
    for line in disassembly.splitlines():
        header = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if header:
            current = found.setdefault(header.group(1), [])
        elif current is not None and re.match(r'^\s*[0-9a-f]+:', line):
            current.append(line.split(':', 1)[1].strip())
    return found


def count_instructions(code: dict[str, list[str]]) -> tuple[dict[str, int], dict[str, int], list[str]]:
    """Count the msr targets and the special instructions; return them with the problems found on the way."""
    writes: dict[str, int] = {}
    special: dict[str, int] = {}
    problems = []
    for name, body in code.items():
        for instruction in body:
            mnemonic = instruction.split()[0] if instruction else ''
            if mnemonic == 'msr':
                target = instruction.split()[1].rstrip(',')
                writes[target] = writes.get(target, 0) + 1
            if mnemonic in FORBIDDEN_MNEMONICS:
                problems.append(f'{name}: forbidden instruction {instruction}')
            if mnemonic in SPECIAL:
                special[mnemonic] = special.get(mnemonic, 0) + 1
            device = DEVICE_IMMEDIATE.search(instruction)
            if device and 0x5000 <= int(device.group(1), 16) < 0x8000 and int(device.group(1), 16) != 0x7000:
                problems.append(f'{name}: device address immediate in code: {instruction}')
    return writes, special, problems


def inspect_disassembly(disassembly: str) -> list[str]:
    code = functions(disassembly)
    writes, special, problems = count_instructions(code)
    for target, count in writes.items():
        if target not in WRITES:
            problems.append(f'unreviewed system register write: msr {target} ({count}x)')
        elif count != WRITES[target][1]:
            problems.append(f'msr {target} appears {count}x, the review lists {WRITES[target][1]}')
    problems += [f'{target} is listed as a write but absent from the code' for target in WRITES if target not in writes]
    for mnemonic, count in special.items():
        if count != SPECIAL[mnemonic]:
            problems.append(f'{mnemonic} appears {count}x, the review lists {SPECIAL[mnemonic]}')
    problems += [f'{mnemonic} is listed but absent from the code' for mnemonic in SPECIAL if mnemonic not in special]
    read32 = code.get('switch_diag_read32')
    if read32 is None or not READ32_BODY.match('\n'.join(read32)):
        problems.append('switch_diag_read32 is not a single load and a return')
    return problems


def report() -> str:
    lines = ['CPU system register writes (llvm-objdump names):']
    lines += [f'  msr {name:<15} x{count}  {why}' for name, (why, count) in WRITES.items()]
    lines += ['Other instructions of note: ' + ', '.join(f'{m} x{n}' for m, n in SPECIAL.items())]
    return '\n'.join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--map', required=True, type=Path)
    parser.add_argument('--disassembly', required=True, type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    problems = inspect_map(args.map.read_text()) + inspect_disassembly(args.disassembly.read_text())
    if args.report:
        args.report.write_text('\n'.join(problems or ['D0 loader inspection passed']) + '\n' + report() + '\n')
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
