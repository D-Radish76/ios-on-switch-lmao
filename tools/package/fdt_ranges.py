"""The memory a flattened device tree reserves, read from the file and not from a running U-Boot.

U-Boot's `fdt rsvmem print` only prints, and `fdt get value` keeps a single cell, so a script cannot compare a live
reservation with an address. The package therefore reads the DTB it copies, at build time, with this reader: the
memory reservation block and the `reg` of every child of `/reserved-memory` that has a size. Entries of size zero (the
Linux framebuffer carveouts, filled in by the OS) are left out.
"""

import struct

FDT_MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


def _cstring(blob: bytes, offset: int) -> str:
    return blob[offset : blob.index(b'\0', offset)].decode()


def _header(dtb: bytes) -> tuple[int, int, int, int, int, int]:
    if len(dtb) < 40:
        raise ValueError('the DTB is shorter than its header')
    magic, total, off_struct, off_strings, off_rsvmap, _version, _last, _cpu, size_strings, size_struct = struct.unpack(
        '>10I', dtb[:40]
    )
    if magic != FDT_MAGIC:
        raise ValueError(f'bad FDT magic {magic:#x}')
    if total > len(dtb) or off_struct + size_struct > total or off_strings + size_strings > total:
        raise ValueError('the DTB header points outside the file')
    return total, off_struct, size_struct, off_strings, off_rsvmap, size_strings


def _memreserve(dtb: bytes, at: int, total: int) -> list[tuple[str, int, int]]:
    found: list[tuple[str, int, int]] = []
    while True:
        if at + 16 > total:
            raise ValueError('the memory reservation block is not terminated')
        address, size = struct.unpack('>QQ', dtb[at : at + 16])
        at += 16
        if address == 0 and size == 0:
            return found
        found.append(('memreserve', address, address + size))


def _properties(dtb: bytes, at: int, end: int, strings: int) -> dict[str, dict[str, bytes]]:
    path: list[str] = []
    props: dict[str, dict[str, bytes]] = {}
    while at < end:
        (token,) = struct.unpack('>I', dtb[at : at + 4])
        at += 4
        if token == BEGIN_NODE:
            name = _cstring(dtb, at)
            at = (at + len(name) + 1 + 3) & ~3
            path.append(name)
        elif token == END_NODE:
            path.pop()
        elif token == PROP:
            length, name_offset = struct.unpack('>II', dtb[at : at + 8])
            value = dtb[at + 8 : at + 8 + length]
            at = (at + 8 + length + 3) & ~3
            props.setdefault('/'.join(path) or '/', {})[_cstring(dtb, strings + name_offset)] = value
        elif token == END:
            break
        elif token != NOP:
            raise ValueError(f'unknown FDT token {token:#x}')
    return props


def _cell(node: dict[str, bytes], name: str, default: int) -> int:
    return struct.unpack('>I', node.get(name, struct.pack('>I', default)))[0]


def _join(words: tuple[int, ...]) -> int:
    value = 0
    for word in words:
        value = (value << 32) | word
    return value


def _carveouts(props: dict[str, dict[str, bytes]]) -> list[tuple[str, int, int]]:
    root = props.get('', {}) or props.get('/', {})
    parent = props.get('/reserved-memory', {})
    address_cells = _cell(parent, '#address-cells', _cell(root, '#address-cells', 2))
    size_cells = _cell(parent, '#size-cells', _cell(root, '#size-cells', 2))
    stride = 4 * (address_cells + size_cells)
    found: list[tuple[str, int, int]] = []
    for where, node in props.items():
        reg = node.get('reg')
        if not where.startswith('/reserved-memory/') or reg is None:
            continue
        if stride == 0 or len(reg) % stride:
            raise ValueError(f'{where}: reg is not a whole number of ({address_cells}+{size_cells}) cell entries')
        for entry in range(0, len(reg), stride):
            words = struct.unpack(f'>{address_cells + size_cells}I', reg[entry : entry + stride])
            address, size = _join(words[:address_cells]), _join(words[address_cells:])
            if size:
                found.append((where, address, address + size))
    return found


def reserved_ranges(dtb: bytes) -> list[tuple[str, int, int]]:
    """Return `(origin, start, end)` for every non-empty reservation, or raise ValueError for a malformed tree."""
    total, off_struct, size_struct, off_strings, off_rsvmap, _size_strings = _header(dtb)
    found = _memreserve(dtb, off_rsvmap, total)
    props = _properties(dtb, off_struct, off_struct + size_struct, off_strings)
    return found + _carveouts(props)
