"""The memory a flattened device tree reserves, read from the file and not from a running U-Boot.

U-Boot's `fdt rsvmem print` only prints, and `fdt get value` keeps a single cell, so a script cannot compare a live
reservation with an address. The package therefore reads the DTB it copies, at build time, with this reader: the
memory reservation block and the `reg` of every child of `/reserved-memory` that has a size. Entries of size zero (the
Linux framebuffer carveouts, filled in by the OS) are left out.

The reader refuses what it does not understand, with ValueError, and never returns a partial list: a bad header, a
block outside the file or overlapping another, a structure block that does not end in FDT_END with every node closed,
a property or name that leaves its block, a repeated node or property, cell counts other than one or two, a missing or
non-empty `ranges` under `/reserved-memory`, a grandchild or a child without `reg` there, and any range past 2**64.
Entries of nodes marked disabled still count: the check errs towards refusing.
"""

import struct

FDT_MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9
HEADER_SIZE = 40
RESERVED = '/reserved-memory'
LIMIT = 1 << 64


class _Blocks:
    def __init__(self, dtb: bytes) -> None:
        if len(dtb) < HEADER_SIZE:
            raise ValueError('the DTB is shorter than its header')
        fields = struct.unpack('>10I', dtb[:HEADER_SIZE])
        magic, self.total, self.off_struct, self.off_strings, self.off_rsvmap = fields[:5]
        version, last_compatible, _cpu, self.size_strings, self.size_struct = fields[5:]
        if magic != FDT_MAGIC:
            raise ValueError(f'bad FDT magic {magic:#x}')
        if version < 17 or last_compatible > 17:
            raise ValueError(f'FDT version {version} (compatible with {last_compatible}) has no structure size field')
        if not HEADER_SIZE <= self.total <= len(dtb):
            raise ValueError('the header total size is outside the file')
        if self.off_rsvmap % 8 or self.off_struct % 4:
            raise ValueError('a block is not aligned')
        if self.size_struct == 0 or self.size_struct % 4:
            raise ValueError('the structure block is empty or not a whole number of words')
        self.dtb = dtb

    def span(self, name: str, start: int, size: int) -> tuple[str, int, int]:
        if start < HEADER_SIZE or start + size > self.total:
            raise ValueError(f'the {name} block is outside the DTB')
        return name, start, start + size


def _check_disjoint(spans: list[tuple[str, int, int]]) -> None:
    for i, (name, start, end) in enumerate(spans):
        for other, other_start, other_end in spans[i + 1 :]:
            if start < other_end and other_start < end:
                raise ValueError(f'the {name} block overlaps the {other} block')


def _memreserve(blocks: _Blocks) -> tuple[list[tuple[str, int, int]], int]:
    found: list[tuple[str, int, int]] = []
    at = blocks.off_rsvmap
    while True:
        if at + 16 > blocks.total:
            raise ValueError('the memory reservation block is not terminated')
        address, size = struct.unpack('>QQ', blocks.dtb[at : at + 16])
        at += 16
        if address == 0 and size == 0:
            return found, at - blocks.off_rsvmap
        if address + size > LIMIT:
            raise ValueError(f'memory reservation {address:#x}+{size:#x} passes 2**64')
        if size:
            found.append(('memreserve', address, address + size))


def _name(blob: bytes, start: int, limit: int, what: str) -> tuple[str, int]:
    end = blob.find(b'\0', start, limit)
    if end < 0:
        raise ValueError(f'a {what} is not terminated inside its block')
    return blob[start:end].decode('utf-8', 'replace'), end + 1


def _property(blocks: _Blocks, at: int, end: int, node: dict[str, bytes] | None) -> int:
    if at + 8 > end:
        raise ValueError('a property header leaves the structure block')
    length, name_offset = struct.unpack('>II', blocks.dtb[at : at + 8])
    value_at = at + 8
    if value_at + length > end or name_offset >= blocks.size_strings:
        raise ValueError('a property leaves its block')
    strings = blocks.off_strings
    name, _ = _name(blocks.dtb, strings + name_offset, strings + blocks.size_strings, 'property name')
    if node is None:
        raise ValueError('a property outside every node')
    if name in node:
        raise ValueError(f'property {name} appears twice in one node')
    node[name] = blocks.dtb[value_at : value_at + length]
    return (value_at + length + 3) & ~3


def _open_node(blocks: _Blocks, at: int, end: int, path: list[str], props: dict[str, dict[str, bytes]]) -> int:
    name, after = _name(blocks.dtb, at, end, 'node name')
    if not path and props:
        raise ValueError('the structure block has a second root node')
    path.append(name)
    key = '/'.join(path) or '/'
    if key in props:
        raise ValueError(f'node {key} appears twice')
    props[key] = {}
    return (after + 3) & ~3


def _tree(blocks: _Blocks) -> dict[str, dict[str, bytes]]:
    blob = blocks.dtb
    at, end = blocks.off_struct, blocks.off_struct + blocks.size_struct
    path: list[str] = []
    props: dict[str, dict[str, bytes]] = {}
    while at < end:
        (token,) = struct.unpack('>I', blob[at : at + 4])
        at += 4
        if token == BEGIN_NODE:
            at = _open_node(blocks, at, end, path, props)
        elif token == END_NODE:
            if not path:
                raise ValueError('FDT_END_NODE with no node open')
            path.pop()
        elif token == PROP:
            at = _property(blocks, at, end, props['/'.join(path) or '/'] if path else None)
        elif token == END:
            if path or at != end:
                raise ValueError('FDT_END with a node still open or bytes after it')
            return props
        elif token != NOP:
            raise ValueError(f'unknown FDT token {token:#x}')
    raise ValueError('the structure block does not end in FDT_END')


def _cells(node: dict[str, bytes], name: str) -> int:
    value = node.get(name)
    if value is None or len(value) != 4:
        raise ValueError(f'{RESERVED} needs a one word {name}')
    (count,) = struct.unpack('>I', value)
    if count not in (1, 2):
        raise ValueError(f'{RESERVED} {name} is {count}; only 1 or 2 fit in 64 bits')
    return count


def _join(words: tuple[int, ...]) -> int:
    value = 0
    for word in words:
        value = (value << 32) | word
    return value


def _entries(where: str, node: dict[str, bytes], cells: tuple[int, int]) -> list[tuple[str, int, int]]:
    reg = node.get('reg')
    if reg is None:
        raise ValueError(f'{where} has no reg: dynamic reservations are not supported')
    address_cells, size_cells = cells
    stride = 4 * (address_cells + size_cells)
    if len(reg) % stride:
        raise ValueError(f'{where}: reg is not a whole number of ({address_cells}+{size_cells}) cell entries')
    found: list[tuple[str, int, int]] = []
    for entry in range(0, len(reg), stride):
        words = struct.unpack(f'>{address_cells + size_cells}I', reg[entry : entry + stride])
        address, size = _join(words[:address_cells]), _join(words[address_cells:])
        if address + size > LIMIT:
            raise ValueError(f'{where}: {address:#x}+{size:#x} passes 2**64')
        if size:
            found.append((where, address, address + size))
    return found


def _carveouts(props: dict[str, dict[str, bytes]]) -> list[tuple[str, int, int]]:
    parent = props.get(RESERVED)
    if parent is None:
        return []
    cells = (_cells(parent, '#address-cells'), _cells(parent, '#size-cells'))
    if parent.get('ranges') != b'':
        raise ValueError(f'{RESERVED} needs an empty ranges property: other mappings are not supported')
    found: list[tuple[str, int, int]] = []
    for where, node in props.items():
        if not where.startswith(RESERVED + '/'):
            continue
        if where.count('/') != RESERVED.count('/') + 1:
            raise ValueError(f'{where}: nested nodes under {RESERVED} are not supported')
        found += _entries(where, node, cells)
    return found


def reserved_ranges(dtb: bytes) -> list[tuple[str, int, int]]:
    """Return `(origin, start, end)` for every non-empty reservation, or raise ValueError for what it cannot read."""
    blocks = _Blocks(dtb)
    memreserve, reserve_bytes = _memreserve(blocks)
    _check_disjoint(
        [
            blocks.span('memory reservation', blocks.off_rsvmap, reserve_bytes),
            blocks.span('structure', blocks.off_struct, blocks.size_struct),
            blocks.span('strings', blocks.off_strings, blocks.size_strings),
            ('header', 0, HEADER_SIZE),
        ]
    )
    return memreserve + _carveouts(_tree(blocks))
