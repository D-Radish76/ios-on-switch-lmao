#!/usr/bin/env python3
"""What the D0 loader's binary may contain, read from its link map and its disassembly.

    diag_loader_inspect.py --map MAP --disassembly DIS [--report OUT] [--sd | --t1 | --t1r | --t1v3]

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
# D1 links the same-boot SD reference validator and its reporting; nothing else of the storage path.
SD_SYMBOLS = ('switch_sd_card_reference_validate', 'switch_diag_sd_evaluate')
# The validator compares the controller address 0x700b0000 with the record's field and never uses it as an address;
# these are the only functions that may hold it as an immediate, and only in comparisons.
SD_CONTROLLER_HOLDERS = ('validate_reference', 'switch_sd_card_reference_validate', 'switch_sd_reference_validate')
# Constants of the validator that look like device addresses to DEVICE_IMMEDIATE: 0x76dc4190, a word of the CRC32 loop,
# and 0x52445349, the reference page's magic.
SD_CONSTANTS = {0x76DC, 0x5244}
SD_FUNCTIONS = ('switch_sd_crc32', *SD_CONTROLLER_HOLDERS)
# T1 links the I2C1 check (boot/diag/loader/t1*.c). Its device accesses all run through switch_t1_access, whose
# allowlist (t1_regs.c) refuses any register or value T1 does not list; the raw read and write are the only code that
# uses an address.
T1_SYMBOLS = ('switch_t1_run', 'switch_t1_access', 'switch_t1_read32', 'switch_t1_write32', 'switch_t1_may_write')
T1_GATE = 'switch_t1_access'
T1_WRITE32_BODY = re.compile(r'\A(?:mov\s+w(\d+), w0\s*\nstr\s+w1, \[x\1\]|str\s+w1, \[x0\])\s*\nret\s*\Z')
T1_CALL = re.compile(r'\b(?:bl|b)\s+0x[0-9a-f]+ <(switch_t1_read32|switch_t1_write32)>')
# T1 v3 (boot/diag/loader/t1v3*.c) has the same shape as T1 under its own names and links nothing of T1 or T1-R.
T1V3_SYMBOLS = (
    'switch_t1v3_run',
    'switch_t1v3_access',
    'switch_t1v3_read32',
    'switch_t1v3_write32',
    'switch_t1v3_may_write',
)
T1V3_GATE = 'switch_t1v3_access'
T1V3_CALL = re.compile(r'\b(?:bl|b)\s+0x[0-9a-f]+ <(switch_t1v3_read32|switch_t1v3_write32)>')
# T1-R (boot/diag/loader/t1r*.c) only reads. It links no write counterpart of the raw read, and the code that forms or
# passes a device address (the allowlist, the gate, the raw read) holds no store except the gate's own stack saves.
T1R_SYMBOLS = ('switch_t1r_run', 'switch_t1r_access', 'switch_t1r_read32', 'switch_t1r_may_read', 'switch_t1r_address')
T1R_GATE = 'switch_t1r_access'
T1R_RAW = 'switch_t1r_read32'
T1R_CALL = re.compile(r'\b(?:bl|b)\s+0x[0-9a-f]+ <switch_t1r_read32>')
T1R_ADDRESS_HOLDERS = ('switch_t1r_address', 'switch_t1r_may_read', 'switch_t1r_hekate_source', 'switch_t1r_read32')
STORE = re.compile(r'^(?:str|stp|stur|stlr|stxr|stnp|st[1-4])[a-z]*\b')
STACK_STORE = re.compile(r'\[(?:sp|x29)\b')
COMPARISONS = ('cmp', 'ccmp', 'cmn', 'ccmn', 'subs')
# The one place a device register address is formed. It loads and returns.
READ32_BODY = re.compile(r'\A(?:mov\s+w\d+, w0\s*\n)?ldr\s+w0, \[x\d+\]\s*\nret\s*\Z')
# Device addresses built as an immediate in code: 0x5000xxxx to 0x7fffxxxx, except the identity registers (0x7000....).
DEVICE_IMMEDIATE = re.compile(r'\b(?:movk|movz|mov)\s+[wx]\d+, #0x([0-9a-f]{4}), lsl #16')
# The same, as one `mov` of a 32 bit value: 0x50000000 to 0x7fffffff. Only the D1 inspection looks for it.
WHOLE_IMMEDIATE = re.compile(r'\bmov\s+[wx]\d+, #0x([0-9a-f]{8})\b')


# Every 32 bit word of the T1 v3 image that lies in the CAR or I2C1 page is one of these, once. They are the allowlist's
# table (boot/diag/loader/t1v3_regs.c) as the linker laid it out, so an address added by any other file shows.
T1V3_PAGES = ((0x60006000, 0x60007000), (0x7000C000, 0x7000D000))
T1V3_ADDRESSES = frozenset(
    {
        0x60006004, 0x60006010, 0x60006124, 0x600060A0,
        0x7000C000, 0x7000C004, 0x7000C00C, 0x7000C01C, 0x7000C068, 0x7000C06C, 0x7000C08C, 0x7000C094, 0x7000C098,
    }
)  # fmt: skip


def inspect_map(text: str, sd: bool = False, t1: bool = False, t1r: bool = False, t1v3: bool = False) -> list[str]:
    forbidden = [name for name in FORBIDDEN_SYMBOLS if not (sd and name == 'switch_sd_')]
    problems = [f'the link has {name}' for name in forbidden if name in text]
    wanted = (
        *REQUIRED_SYMBOLS,
        *(SD_SYMBOLS if sd else ()),
        *(T1_SYMBOLS if t1 else ()),
        *(T1R_SYMBOLS if t1r else ()),
        *(T1V3_SYMBOLS if t1v3 else ()),
    )
    problems += [f'the link lacks {name}' for name in wanted if name not in text]
    problems += [f'the link has {name} outside T1' for name in T1_SYMBOLS if not t1 and name in text]
    problems += [f'the link has {name} outside T1-R' for name in T1R_SYMBOLS if not t1r and name in text]
    problems += [f'the link has {name} outside T1 v3' for name in T1V3_SYMBOLS if not t1v3 and name in text]
    if t1r:
        problems += [f'T1-R links {name}' for name in ('switch_t1r_write', 'switch_t1_') if name in text]
    if t1v3:
        problems += [f'T1 v3 links {name}' for name in ('switch_t1_', 'switch_t1r_') if name in text]
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


def controller_page_immediates(name: str, body: list[str]) -> list[str]:
    """Return the functions' `mov` of a value inside the SD controller's page; only the validator's compare may."""
    return [
        f'{name}: the SD controller address is formed in code: {instruction}'
        for instruction in body
        if (m := WHOLE_IMMEDIATE.search(instruction))
        and m.group(1).startswith('700b0')
        and not (name in SD_CONTROLLER_HOLDERS and m.group(1) == '700b0000')
    ]


def controller_uses(name: str, body: list[str]) -> list[str]:
    """Return what a function does with a register holding 0x700b0000 other than compare it.

    The immediate is built as `movz`/`movk ... #0x700b, lsl #16`; the register is tracked until it is written again.
    A load or store through it, or a copy of it, is a path to the SD controller by a formed address.
    """
    problems, held = [], None
    for instruction in body:
        mnemonic = instruction.split()[0] if instruction else ''
        built = re.match(
            r'(?:(?:movk|movz|mov)\s+([wx])(\d+), #0x700b, lsl #16|mov\s+([wx])(\d+), #0x700b0000\b)', instruction
        )
        if built:
            held = built.group(2) or built.group(4)
            continue
        if held is None or not re.search(rf'\b[wx]{held}\b', instruction):
            continue
        if re.match(rf'{mnemonic}\s+[wx]{held}\b', instruction) and mnemonic not in COMPARISONS:
            held = None
        elif mnemonic not in COMPARISONS:
            problems.append(f'{name}: the controller address is used other than in a comparison: {instruction}')
    return problems


def count_instructions(
    code: dict[str, list[str]], sd: bool = False
) -> tuple[dict[str, int], dict[str, int], list[str]]:
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
                high = int(device.group(1), 16)
                if (
                    sd
                    and name in SD_FUNCTIONS
                    and (high in SD_CONSTANTS or (high == 0x700B and name != SD_FUNCTIONS[0]))
                ):
                    continue
                problems.append(f'{name}: device address immediate in code: {instruction}')
        if sd:
            problems += controller_page_immediates(name, body)
            if name in SD_CONTROLLER_HOLDERS:
                problems += controller_uses(name, body)
    return writes, special, problems


def inspect_t1(code: dict[str, list[str]], prefix: str = 'switch_t1_') -> list[str]:
    """Return what is wrong with a raw access pair: one instruction each, called only by the gate.

    prefix is switch_t1_ for T1 and switch_t1v3_ for T1 v3.
    """
    gate, call_pattern = (T1_GATE, T1_CALL) if prefix == 'switch_t1_' else (T1V3_GATE, T1V3_CALL)
    problems = []
    write32, read32 = code.get(f'{prefix}write32'), code.get(f'{prefix}read32')
    if write32 is None or not T1_WRITE32_BODY.match('\n'.join(write32)):
        problems.append(f'{prefix}write32 is not a single store and a return')
    if read32 is None or not READ32_BODY.match('\n'.join(read32)):
        problems.append(f'{prefix}read32 is not a single load and a return')
    for name, body in code.items():
        if name == gate:
            continue
        problems += [
            f'{name}: calls {call.group(1)} outside {gate}' for line in body if (call := call_pattern.search(line))
        ]
    if not any(call_pattern.search(line) for line in code.get(gate, [])):
        problems.append(f'{gate} does not reach the raw accesses')
    return problems


def inspect_t1r(code: dict[str, list[str]]) -> list[str]:
    """Return what is wrong with T1-R's device reads: one load, one caller, and no store on any address path."""
    problems = []
    read32 = code.get(T1R_RAW)
    if read32 is None or not READ32_BODY.match('\n'.join(read32)):
        problems.append(f'{T1R_RAW} is not a single load and a return')
    for name, body in code.items():
        if name != T1R_GATE:
            problems += [f'{name}: calls {T1R_RAW} outside {T1R_GATE}' for line in body if T1R_CALL.search(line)]
    if not any(T1R_CALL.search(line) for line in code.get(T1R_GATE, [])):
        problems.append(f'{T1R_GATE} does not reach the raw read')
    for name in (T1R_GATE, *T1R_ADDRESS_HOLDERS):
        for line in code.get(name, []):
            if STORE.match(line) and not STACK_STORE.search(line):
                problems.append(f'{name}: a store that is not a stack save: {line}')
            if name != T1R_GATE and STORE.match(line):
                problems.append(f'{name}: holds a store: {line}')
    return problems


def inspect_image(flat: bytes, t1v3: bool = False) -> list[str]:
    """Return what is wrong with the linked flat image: for T1 v3, the register addresses it holds as data."""
    if not t1v3:
        return []
    found = [
        word
        for offset in range(0, len(flat) - 3, 4)
        if any(low <= (word := int.from_bytes(flat[offset : offset + 4], 'little')) < high for low, high in T1V3_PAGES)
    ]
    problems = [
        f'T1 v3 image holds the address {word:#x}, which is not listed' for word in sorted(set(found) - T1V3_ADDRESSES)
    ]
    problems += [f'T1 v3 image lacks the listed address {word:#x}' for word in sorted(T1V3_ADDRESSES - set(found))]
    problems += [
        f'T1 v3 image holds {word:#x} {found.count(word)} times' for word in sorted(set(found)) if found.count(word) > 1
    ]
    return problems


def inspect_disassembly(
    disassembly: str, sd: bool = False, t1: bool = False, t1r: bool = False, t1v3: bool = False
) -> list[str]:
    code = functions(disassembly)
    writes, special, problems = count_instructions(code, sd)
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
    if t1:
        problems += inspect_t1(code)
    if t1r:
        problems += inspect_t1r(code)
    if t1v3:
        problems += inspect_t1(code, 'switch_t1v3_')
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
    parser.add_argument('--sd', action='store_true', help='the D1 image: the SD reference validator is linked')
    parser.add_argument('--t1', action='store_true', help='the T1 image: the I2C1 check is linked')
    parser.add_argument('--t1r', action='store_true', help='the T1-R image: the read-only clock check is linked')
    parser.add_argument('--t1v3', action='store_true', help='the T1 v3 image: the divisor test is linked')
    args = parser.parse_args()
    problems = inspect_map(args.map.read_text(), args.sd, args.t1, args.t1r, args.t1v3) + inspect_disassembly(
        args.disassembly.read_text(), args.sd, args.t1, args.t1r, args.t1v3
    )
    if args.report:
        args.report.write_text('\n'.join(problems or ['D0 loader inspection passed']) + '\n' + report() + '\n')
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
