"""What U-Boot's `bdinfo` does, read from the pinned bl33.bin's own machine code.

The command's function is found through the code that loads its first label, `arch_number`. It may read the global
data and the board info and call two functions: the one that prints a label and a value, and printf. It may store only
to its own stack frame and may not use a system instruction.
"""

import re
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

LABEL = b'arch_number\0'
STORES = ('str', 'stp', 'stur', 'strb', 'strh', 'sturb', 'sturh', 'stnp', 'stlr', 'stxr', 'stlxr', 'st1', 'swp', 'cas')
SYSTEM = ('svc', 'hvc', 'smc', 'msr', 'dc', 'ic', 'tlbi', 'blr', 'br', 'sys', 'eret')
LINE = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*?)\s*(?://.*)?$')
FRAME = re.compile(r'\[(sp|x29)\b')


@dataclass(frozen=True)
class Instruction:
    address: int
    mnemonic: str
    operands: str


def wrap_elf(code: bytes) -> bytes:
    """Return a relocatable AArch64 ELF whose .text is `code`, so a disassembler can read a raw image."""
    names = b'\0.text\0.shstrtab\0'
    text_at = 64
    names_at = text_at + len(code)
    headers_at = (names_at + len(names) + 7) & ~7
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    ehdr = ident + struct.pack('<HHIQQQIHHHHHH', 1, 183, 1, 0, 0, headers_at, 0, 64, 0, 0, 64, 3, 2)
    text = struct.pack('<IIQQQQIIQQ', 1, 1, 6, 0, text_at, len(code), 0, 0, 4, 0)
    shstrtab = struct.pack('<IIQQQQIIQQ', 7, 3, 0, 0, names_at, len(names), 0, 0, 1, 0)
    return ehdr + code + names + bytes(headers_at - names_at - len(names)) + bytes(64) + text + shstrtab


def disassemble(code: bytes, objdump: str) -> str:
    with tempfile.TemporaryDirectory() as scratch:
        image = Path(scratch) / 'image.o'
        image.write_bytes(wrap_elf(code))
        done = subprocess.run(
            [objdump, '-d', '--triple=aarch64', str(image)], capture_output=True, text=True, check=False
        )
    if done.returncode:
        raise RuntimeError(done.stderr)
    return done.stdout


def parse(listing: str) -> list[Instruction]:
    found = []
    for line in listing.splitlines():
        match = LINE.match(line)
        if match:
            found.append(Instruction(int(match.group(1), 16), match.group(2), match.group(3)))
    return found


def label_users(code: bytes, instructions: list[Instruction], label: bytes = LABEL) -> list[int]:
    """Return the indexes of the instructions that form the address of `label` with an adrp and an add."""
    target = code.find(label)
    if target < 0:
        raise ValueError('the label is not in the image')
    users = []
    for i, instruction in enumerate(instructions):
        page = re.match(r'(x\d+), 0x([0-9a-f]+)(?: <.*>)?$', instruction.operands)
        if instruction.mnemonic != 'adrp' or not page:
            continue
        for follower in instructions[i + 1 : i + 6]:
            offset = re.match(rf'x\d+, {page.group(1)}, #0x([0-9a-f]+)$', follower.operands)
            if follower.mnemonic == 'add' and offset and int(page.group(2), 16) + int(offset.group(1), 16) == target:
                users.append(i)
    return users


def bdinfo_function(code: bytes, instructions: list[Instruction]) -> list[Instruction]:
    users = label_users(code, instructions)
    if len(users) != 1:
        raise ValueError(f'{len(users)} places load the label, expected one')
    start = users[0]
    while not (instructions[start].mnemonic == 'stp' and instructions[start].operands.startswith('x29, x30, [sp, #-')):
        start -= 1
    end = start
    while instructions[end].mnemonic != 'ret':
        end += 1
    return instructions[start : end + 1]


def violations(function: list[Instruction]) -> list[str]:
    problems = []
    for instruction in function:
        text = f'{instruction.address:#x}: {instruction.mnemonic} {instruction.operands}'
        if instruction.mnemonic in SYSTEM:
            problems.append(f'system instruction {text}')
        if instruction.mnemonic in STORES and not FRAME.search(instruction.operands):
            problems.append(f'store outside the stack frame {text}')
    return problems


def calls(function: list[Instruction]) -> set[int]:
    return {int(i.operands.split()[0], 16) for i in function if i.mnemonic == 'bl'}
