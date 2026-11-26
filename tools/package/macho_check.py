"""Structural check of the kernel's Mach-O: segments, protections, entry and the boot window it will be placed in.

    first_xnu_check.py macho KERNEL [--base VA] [--phys-base PA] [--entry-phys PA] [--compare OTHER]

It refuses what no kernel may have (a segment both writable and executable, overlapping ranges, a segment outside
the file, an entry outside executable file-backed memory) and reports the rest. Empty segments (vmsize 0, as
-sectcreate from /dev/null leaves), zerofill (filesize below vmsize) and an unaligned file offset are normal Mach-O and are not
refused. --compare lists what differs between two kernels: a difference is reported, never judged.
"""

import itertools
import struct
from dataclasses import dataclass
from pathlib import Path

import native_xnu_layout as layout

MAGIC, CPU_ARM64, KIND_EXECUTE = 0xFEEDFACF, 0x0100000C, 2
LC_SEGMENT_64, LC_SYMTAB, LC_UNIXTHREAD = 0x19, 0x2, 0x5
ARM_THREAD_STATE64, ARM_THREAD_STATE64_COUNT = 6, 68
PC_OFFSET = 272
VM_PAGE = 0x4000
READ, WRITE, EXEC = 1, 2, 4
START_SYMBOL = b'__start'
DEFAULT_PHYS_BASE = 0xB207C000


class MachoError(ValueError):
    pass


@dataclass(frozen=True)
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    maxprot: int
    initprot: int

    @property
    def empty(self) -> bool:
        return self.vmsize == 0


@dataclass(frozen=True)
class Macho:
    segments: tuple[Segment, ...]
    entry: int
    start_symbol: int | None
    size: int


def protection(bits: int) -> str:
    return ''.join(c if bits & b else '-' for c, b in (('r', READ), ('w', WRITE), ('x', EXEC)))


def _segment(data: bytes, at: int, length: int) -> Segment:
    if length < 72:
        raise MachoError('a segment command shorter than its fixed part')
    name = data[at + 8 : at + 24].split(b'\0', 1)[0].decode('ascii', errors='backslashreplace')
    vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, at + 24)
    maxprot, initprot = struct.unpack_from('<2I', data, at + 56)
    return Segment(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot)


def _start_symbol(data: bytes, at: int) -> int | None:
    symoff, count, stroff, strsize = struct.unpack_from('<4I', data, at + 8)
    if stroff + strsize > len(data) or symoff + 16 * count > len(data):
        raise MachoError('the symbol table lies outside the file')
    strings = data[stroff : stroff + strsize]
    needle = b'\0' + START_SYMBOL + b'\0'
    found = strings.find(needle)
    if found < 0:
        return None
    index = found + 1
    for strx, _kind, _sect, _desc, value in struct.iter_unpack('<IBBHQ', data[symoff : symoff + 16 * count]):
        if strx == index:
            return value
    return None


def parse(data: bytes) -> Macho:
    if len(data) < 32 or struct.unpack_from('<2I', data) != (MAGIC, CPU_ARM64):
        raise MachoError('not a little-endian arm64 Mach-O')
    kind, count, size = struct.unpack_from('<3I', data, 12)
    if kind != KIND_EXECUTE or 32 + size > len(data):
        raise MachoError('not an executable Mach-O, or its load commands exceed the file')
    cursor, segments, entries, start = 32, [], [], None
    for _ in range(count):
        if 32 + size - cursor < 8:
            raise MachoError('a truncated load command')
        command, length = struct.unpack_from('<II', data, cursor)
        if length < 8 or length % 8 or cursor + length > 32 + size:
            raise MachoError('a load command with an impossible length')
        if command == LC_SEGMENT_64:
            segments.append(_segment(data, cursor, length))
        elif command == LC_UNIXTHREAD:
            if length != 288 or struct.unpack_from('<II', data, cursor + 8) != (
                ARM_THREAD_STATE64,
                ARM_THREAD_STATE64_COUNT,
            ):
                raise MachoError('an LC_UNIXTHREAD that is not one ARM64 thread state')
            entries.append(struct.unpack_from('<Q', data, cursor + PC_OFFSET)[0])
        elif command == LC_SYMTAB:
            start = _start_symbol(data, cursor)
        cursor += length
    if len(entries) != 1 or not segments:
        raise MachoError('expected exactly one LC_UNIXTHREAD and at least one segment')
    return Macho(tuple(segments), entries[0], start, len(data))


def _protections(segment: Segment) -> list[str]:
    problems = []
    if segment.initprot & ~segment.maxprot:
        problems.append(
            f'{segment.name}: initprot {protection(segment.initprot)} is not within maxprot {protection(segment.maxprot)}'
        )
    for label, bits in (('initprot', segment.initprot), ('maxprot', segment.maxprot)):
        if bits & WRITE and bits & EXEC:
            problems.append(f'{segment.name}: {label} {protection(bits)} is writable and executable')
    return problems


def _ranges(kind: str, spans: list[tuple[str, int, int]]) -> list[str]:
    ordered = sorted((s for s in spans if s[2] > s[1]), key=lambda s: s[1])
    return [f'{kind}: {a[0]} and {b[0]} overlap' for a, b in itertools.pairwise(ordered) if a[2] > b[1]]


def problems(image: Macho) -> list[str]:
    found = []
    names = [s.name for s in image.segments]
    if len(set(names)) != len(names):
        found.append('two segments share a name')
    for s in image.segments:
        found += _protections(s)
        if s.filesize > s.vmsize:
            found.append(f'{s.name}: filesize {s.filesize:#x} exceeds vmsize {s.vmsize:#x}')
        if s.filesize and s.fileoff + s.filesize > image.size:
            found.append(f'{s.name}: file range ends at {s.fileoff + s.filesize:#x}, past the file ({image.size:#x})')
        if not s.empty and (s.vmaddr % VM_PAGE or s.vmsize % VM_PAGE):
            found.append(f'{s.name}: vmaddr {s.vmaddr:#x} or vmsize {s.vmsize:#x} is not a multiple of {VM_PAGE:#x}')
    found += _ranges('virtual', [(s.name, s.vmaddr, s.vmaddr + s.vmsize) for s in image.segments])
    found += _ranges('file', [(s.name, s.fileoff, s.fileoff + s.filesize) for s in image.segments])
    found += _linkedit(image)
    found += _entry(image)
    return found


def _linkedit(image: Macho) -> list[str]:
    edit = [s for s in image.segments if s.name == '__LINKEDIT']
    if len(edit) != 1:
        return ['there is not exactly one __LINKEDIT']
    found = []
    if image.segments[-1] is not edit[0]:
        found.append('__LINKEDIT is not the last load command')
    return found


def notes(image: Macho) -> list[str]:
    """What is normal but worth reading: nothing here fails the check."""
    found = []
    edit = next((s for s in image.segments if s.name == '__LINKEDIT'), None)
    if edit is not None:
        if any(s.vmaddr + s.vmsize > edit.vmaddr for s in image.segments if s is not edit and not s.empty):
            found.append('a segment ends above the start of __LINKEDIT in memory')
        if any(s.fileoff + s.filesize > edit.fileoff for s in image.segments if s is not edit and s.filesize):
            found.append('a segment ends above the start of __LINKEDIT in the file')
        if edit.fileoff + edit.filesize != image.size:
            found.append(f'__LINKEDIT ends at {edit.fileoff + edit.filesize:#x}, the file at {image.size:#x}')
    found += [f'{s.name}: empty (vmsize 0)' for s in image.segments if s.empty]
    found += [
        f'{s.name}: zerofill of {s.vmsize - s.filesize:#x} bytes' for s in image.segments if 0 < s.filesize < s.vmsize
    ]
    found += [
        f'{s.name}: file offset {s.fileoff:#x} is not a multiple of {VM_PAGE:#x}'
        for s in image.segments
        if s.filesize and s.fileoff % VM_PAGE
    ]
    if image.start_symbol is None:
        found.append('no __start symbol: the entry was not compared with one')
    return found


def executable_at(image: Macho, address: int) -> Segment | None:
    for s in image.segments:
        if s.vmaddr <= address < s.vmaddr + s.filesize and s.initprot & EXEC:
            return s
    return None


def _entry(image: Macho) -> list[str]:
    found = []
    if image.entry % 4:
        found.append(f'entry {image.entry:#x} is not 4-byte aligned')
    if executable_at(image, image.entry) is None:
        found.append(f"entry {image.entry:#x} is not in an executable segment's file-backed range")
    if image.start_symbol is not None and image.start_symbol != image.entry:
        found.append(f'entry {image.entry:#x} differs from __start {image.start_symbol:#x}')
    return found


def window(data: bytes, base: int | None, phys_base: int, entry_phys: int | None) -> list[str]:
    """What the boot package's own layout parser and the kernel's physical placement say about this kernel."""
    try:
        kernel = layout.parse_kernel(data)
    except layout.LayoutError as exc:
        return [f'the layout tool refuses this kernel: {exc}']
    found = []
    if base is not None and kernel.base != base:
        found.append(f'the lowest segment is at {kernel.base:#x}, not {base:#x}')
    if (kernel.base - phys_base) % layout.BLOCK:
        found.append(f"physical base {phys_base:#x} is not at the lowest segment's offset modulo 2 MiB")
    physical = phys_base + kernel.entry - kernel.base
    if entry_phys is not None and physical != entry_phys:
        found.append(f'the entry maps to {physical:#x}, not {entry_phys:#x}')
    return found


def table(image: Macho) -> list[str]:
    rows = [f'{"segment":18} {"vmaddr":>18} {"vmsize":>10} {"fileoff":>10} {"filesize":>10} init max']
    rows += [
        f'{s.name:18} {s.vmaddr:#18x} {s.vmsize:#10x} {s.fileoff:#10x} {s.filesize:#10x} {protection(s.initprot)} {protection(s.maxprot)}'
        for s in image.segments
    ]
    seg = executable_at(image, image.entry)
    start = 'absent' if image.start_symbol is None else f'{image.start_symbol:#x}'
    rows.append(f'entry {image.entry:#x} in {seg.name if seg else "no executable segment"}; __start {start}')
    return rows


def differences(left: Macho, right: Macho) -> list[str]:
    lines = []
    if left.entry != right.entry:
        lines.append(f'entry {left.entry:#x} against {right.entry:#x}')
    by_name = {s.name: s for s in right.segments}
    for s in left.segments:
        other = by_name.get(s.name)
        if other is None:
            lines.append(f'{s.name}: only in the first')
        elif (s.vmaddr, s.vmsize, s.fileoff, s.filesize, s.initprot, s.maxprot) != (
            other.vmaddr,
            other.vmsize,
            other.fileoff,
            other.filesize,
            other.initprot,
            other.maxprot,
        ):
            lines.append(
                f'{s.name}: vm {s.vmaddr:#x}+{s.vmsize:#x} file {s.fileoff:#x}+{s.filesize:#x} against '
                f'vm {other.vmaddr:#x}+{other.vmsize:#x} file {other.fileoff:#x}+{other.filesize:#x}'
            )
    lines += [f'{name}: only in the second' for name in by_name if name not in {s.name for s in left.segments}]
    return lines


def check(path: Path, base: int | None, phys_base: int, entry_phys: int | None) -> tuple[Macho, list[str]]:
    data = path.read_bytes()
    image = parse(data)
    return image, problems(image) + window(data, base, phys_base, entry_phys)
