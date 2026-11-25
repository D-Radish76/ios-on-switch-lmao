"""Build small flattened device trees for the reader's tests, with the ways a tree can be wrong left to the caller."""

import struct

STRINGS = b'#address-cells\0#size-cells\0reg\0ranges\0'
NAMES = {'#address-cells': 0, '#size-cells': 15, 'reg': 27, 'ranges': 31}
BEGIN, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


def word(value: int) -> bytes:
    return struct.pack('>I', value)


def prop(name: str, value: bytes = b'', name_offset: int | None = None, length: int | None = None) -> bytes:
    offset = NAMES[name] if name_offset is None else name_offset
    size = len(value) if length is None else length
    return struct.pack('>III', PROP, size, offset) + value + b'\0' * (-len(value) % 4)


def node(name: str, body: bytes = b'', close: bool = True) -> bytes:
    raw = name.encode() + b'\0'
    return word(BEGIN) + raw + b'\0' * (-len(raw) % 4) + body + (word(END_NODE) if close else b'')


def reg(address: int, size: int, cells: tuple[int, int] = (2, 2)) -> bytes:
    def cell(value: int, count: int) -> bytes:
        return struct.pack(f'>{count}I', *[(value >> (32 * i)) & 0xFFFFFFFF for i in reversed(range(count))])

    return cell(address, cells[0]) + cell(size, cells[1])


def reserved(carveouts: dict[str, bytes], cells: tuple[int, int] = (2, 2), ranges: bool = True) -> bytes:
    body = prop('#address-cells', word(cells[0])) + prop('#size-cells', word(cells[1]))
    body += prop('ranges') if ranges else b''
    return node('reserved-memory', body + b''.join(node(n, prop('reg', r)) for n, r in carveouts.items()))


def tree(
    reserve: list[tuple[int, int]],
    carveouts: dict[str, tuple[int, int]],
    *,
    structure: bytes | None = None,
    end: bytes = word(END),
    header: dict[str, int] | None = None,
) -> bytes:
    """Return a minimal valid tree, or one with `structure`, `end` or header fields replaced."""
    body = structure
    if body is None:
        regs = {n: reg(a, z) for n, (a, z) in carveouts.items()}
        body = node('', prop('#address-cells', word(2)) + prop('#size-cells', word(2)) + reserved(regs))
    structure_block = body + end
    reserve_block = b''.join(struct.pack('>QQ', a, z) for a, z in reserve) + struct.pack('>QQ', 0, 0)
    off_rsv = 40
    off_struct = off_rsv + len(reserve_block)
    off_strings = off_struct + len(structure_block)
    fields = {
        'magic': 0xD00DFEED,
        'total': off_strings + len(STRINGS),
        'off_struct': off_struct,
        'off_strings': off_strings,
        'off_rsv': off_rsv,
        'version': 17,
        'last': 16,
        'cpu': 0,
        'size_strings': len(STRINGS),
        'size_struct': len(structure_block),
        **(header or {}),
    }
    packed = struct.pack('>10I', *fields.values())
    return packed + reserve_block + structure_block + STRINGS
