"""What the pinned SD exporter (bazel/pinned/firmware/exporter.uimg) can do, read from its own machine code.

The payload of the U-Boot image is disassembled whole. It may use `dsb sy` and no other system instruction, form no
address of a device register, call only through the reader callback it loads from its own image, and store to its stack,
its own image and the one record page. `RECORD` is the only absolute address the code builds for a store.
"""

import re
import struct
from dataclasses import dataclass

from bdinfo_audit import Instruction, disassemble, parse

HEADER_BYTES = 64
MAGIC = 0x27051956
LINK = 0xA7000000
RECORD = 0xA001F400
CONTROLLER = 0x700B0000
FORBIDDEN = ('svc', 'hvc', 'smc', 'msr', 'mrs', 'dc', 'ic', 'tlbi', 'sys', 'sysl', 'eret', 'wfe', 'wfi', 'sev', 'brk')


@dataclass(frozen=True)
class Image:
    code: bytes
    load: int
    entry: int


def payload(uimage: bytes) -> Image:
    magic, _, _, size, load, entry = struct.unpack('>6I', uimage[:24])
    if magic != MAGIC or len(uimage) < HEADER_BYTES + size:
        raise ValueError('not a U-Boot legacy image')
    return Image(uimage[HEADER_BYTES : HEADER_BYTES + size], load, entry)


def constants(instructions: list[Instruction]) -> set[int]:
    """Every value a movz/movk or mov of an immediate builds, per destination register, over a straight run of code."""
    values: set[int] = set()
    held: dict[str, int] = {}
    for i in instructions:
        reg = i.operands.split(',')[0].strip()
        imm = re.search(r'#(0x[0-9a-f]+|\d+)', i.operands)
        if i.mnemonic in ('mov', 'movz') and imm and not re.match(r'[wx]\d+,\s*[wx]', i.operands.replace('#', '')):
            held[reg] = int(imm.group(1), 0)
            values.add(held[reg])
        elif i.mnemonic == 'movk' and imm and reg in held:
            shift = re.search(r'lsl #(\d+)', i.operands)
            held[reg] |= int(imm.group(1), 0) << (int(shift.group(1)) if shift else 0)
            values.add(held[reg])
        elif i.mnemonic == 'movk' and imm:
            held[reg] = int(imm.group(1), 0) << int(re.search(r'lsl #(\d+)', i.operands).group(1))
    return values


def audit(image: Image, objdump: str) -> dict:
    instructions = parse(disassemble(image.code, objdump))
    mnemonics = [i.mnemonic for i in instructions]
    return {
        'system': {m: mnemonics.count(m) for m in FORBIDDEN if m in mnemonics},
        'barriers': [i.operands for i in instructions if i.mnemonic == 'dsb'],
        'constants': constants(instructions),
        'indirect_calls': sum(m == 'blr' for m in mnemonics),
        'plain_branches_to_registers': sum(m == 'br' for m in mnemonics),
    }
