r"""Read T2's register table and write allowlist from the sources and say what they allow.

    diag_t2_allowlist.py boot/diag/loader

The disassembly audit shows which addresses the linked image holds and who may touch them. This reads what the
table and the allowlist say about each one: the 13 addresses, the CAR read only, the I2C1 writes, the values, and
that the TMP451 one-shot is the one write the bus layer sends (pointer 0x0F, value 0x00). Text only, so the Mac file
works as well.
"""

import re
import sys
from pathlib import Path

CAR, I2C1 = 0x60006000, 0x7000C000
ADDRESSES = frozenset(
    {
        CAR + 0x004, CAR + 0x010, CAR + 0x124, CAR + 0x0A0,
        *(I2C1 + o for o in (0x00, 0x04, 0x0C, 0x1C, 0x68, 0x6C, 0x8C, 0x94, 0x98)),
    }
)  # fmt: skip
WRITABLE = {
    I2C1 + 0x00: {0x2800, 0x2840, 0x2A00, 0x2A40, 0x2802, 0x2A02},
    I2C1 + 0x04: {0x98, 0x99},
    I2C1 + 0x0C: {0xFE, 0x03, 0x11, 0x12, 0x02, 0x01, 0x10, 0x00, 0x15, 0x0F},
    I2C1 + 0x6C: {0x00FF0001},
    I2C1 + 0x8C: {0x01, 0x25},
}
ENTRY = re.compile(r'\[(T2_HW_[A-Z0-9_]+)\]\s*=\s*\{(CAR|I2C1)\s*\+\s*(0x[0-9a-fA-F]+),\s*([RW| ]+)\}')
CASE = re.compile(r'case (T2_HW_[A-Z0-9_]+):(.*?)(?=\n    case |\n    default:)', re.DOTALL)
RETURN_VALUES = re.compile(r'(?:value\s*==\s*|case\s+)(0x[0-9a-fA-F]+|SWITCH_T2_[A-Z_]+)u?')
NAMED = {'SWITCH_T2_DIVISOR_AFTER': 0x00FF0001, 'SWITCH_T2_LOAD_APPLY': 0x01, 'SWITCH_T2_LOAD_TRANSFER': 0x25}


def table(source: str) -> dict[str, tuple[int, set[str]]]:
    found = {}
    for name, base, offset, access in ENTRY.findall(source):
        found[name] = ((CAR if base == 'CAR' else I2C1) + int(offset, 16), {a.strip() for a in access.split('|')})
    return found


def write_values(source: str) -> dict[str, set[int]]:
    """Return, for each register with a case in switch_t2_may_write, the values it accepts."""
    body = source[source.index('int switch_t2_may_write') :]
    found = {}
    for name, text in CASE.findall(body):
        values = set()
        for token in RETURN_VALUES.findall(text):
            values.add(NAMED[token] if token in NAMED else int(token, 16))
        found[name] = values
    return found


def audit(regs_source: str, bus_source: str) -> tuple[list[str], list[str]]:
    """Return the report lines and the problems."""
    entries, writes = table(regs_source), write_values(regs_source)
    report = [f'   {n}: {a:#010x} {"|".join(sorted(acc))}' for n, (a, acc) in entries.items()]
    problems = []
    addresses = [a for a, _ in entries.values()]
    problems += [f'the table lists {a:#x}, which is not one of the 13' for a in sorted(set(addresses) - ADDRESSES)]
    problems += [f'the table lacks {a:#x}' for a in sorted(ADDRESSES - set(addresses))]
    problems += [f'{a:#x} appears twice in the table' for a in sorted(set(addresses)) if addresses.count(a) > 1]
    if len(entries) != 13:
        problems.append(f'the table has {len(entries)} entries, not 13')
    for name, (address, access) in entries.items():
        if CAR <= address < CAR + 0x1000 and access != {'R'}:
            problems.append(f'{name} (CAR {address:#x}) is not read only: {"|".join(sorted(access))}')
        if not access <= {'R', 'W'}:
            problems.append(f'{name} has an unknown access {access}')
    writable = {a: v for n, (a, acc) in entries.items() if 'W' in acc for v in [writes.get(n, set())]}
    problems += [f'{a:#x} is writable and not in the reviewed set' for a in sorted(set(writable) - set(WRITABLE))]
    problems += [f'{a:#x} is not writable and should be' for a in sorted(set(WRITABLE) - set(writable))]
    for address, values in sorted(writable.items()):
        report.append(f'   writes to {address:#010x}: ' + ' '.join(f'{v:#x}' for v in sorted(values)))
        if address in WRITABLE and values != WRITABLE[address]:
            extra, missing = sorted(values - WRITABLE[address]), sorted(WRITABLE[address] - values)
            problems.append(
                f'{address:#x} accepts {[hex(v) for v in extra]} beyond and lacks {[hex(v) for v in missing]} '
                'of the reviewed values'
            )
    one_shot = re.search(r'T2_ONE_SHOT\s*=\s*(0x[0-9a-fA-F]+)', bus_source)
    if not one_shot or int(one_shot.group(1), 16) != 0x0F:
        problems.append('T2_ONE_SHOT is not the TMP451 one-shot start pointer 0x0f')
    if 'reg != T2_ONE_SHOT || value != 0' not in bus_source:
        problems.append('bus_write does not refuse everything but the one-shot (pointer T2_ONE_SHOT, value 0)')
    if bus_source.count('CNFG_WRITE2)') != 1:
        problems.append('the two-byte write is not issued from exactly one place')
    report.append(
        f'   one-shot: pointer {one_shot.group(1) if one_shot else "not found"} (TMP451 one-shot start), '
        'value 0x00, one issuing call'
    )
    return report, problems


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    directory = Path(sys.argv[1])
    bus = (directory / 't2_bus.c').read_text() + (directory.parent / 't2' / 't2.h').read_text()
    report, problems = audit((directory / 't2_regs.c').read_text(), bus)
    print('T2 register table and allowlist\n' + '\n'.join(report))
    if problems:
        print('\nPROBLEMS:\n' + '\n'.join(problems), file=sys.stderr)
        return 1
    print(
        '\nALLOWLIST PASSED: 13 addresses, the four CAR words read only, '
        'writes on five I2C1 registers with the reviewed values.'
    )
    return 0


if __name__ == '__main__':
    sys.exit(main())
