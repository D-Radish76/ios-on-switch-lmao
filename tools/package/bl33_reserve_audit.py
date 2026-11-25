"""Where the pinned bl33.bin can add a reserve-map entry, read from its machine code.

The U-Boot source of this build (2024.NX02) is not in this repository, so the binary is the evidence. Its only callers
of `fdt_add_mem_rsv` are found through the error string that `fdt rsvmem add` prints after it. Nothing here runs code.
"""

import struct

ADD_ERROR = b'libfdt fdt_add_mem_rsv():  %s'
DELETE_ERROR = b'libfdt fdt_del_mem_rsv():  %s'
PROLOGUE = 0xA9807BFD


def words(image: bytes) -> tuple[int, ...]:
    return struct.unpack(f'<{len(image) // 4}I', image[: len(image) // 4 * 4])


def _offset(word: int, bits: int, shift: int = 0) -> int:
    value = (word >> shift) & ((1 << bits) - 1)
    return value - (1 << bits) if value >> (bits - 1) else value


def branch_target(at: int, word: int) -> int | None:
    """Return the target of the B or BL at byte offset `at`, else None."""
    return at + 4 * _offset(word, 26) if word >> 26 in (0b000101, 0b100101) else None


def is_conditional(word: int) -> bool:
    return word & 0xFF000010 == 0x54000000 or word & 0x7E000000 in (0x34000000, 0x36000000)


def conditional_target(at: int, word: int) -> int | None:
    if word & 0xFF000010 == 0x54000000 or word & 0x7E000000 == 0x34000000:
        return at + 4 * _offset(word, 19, 5)
    return at + 4 * _offset(word, 14, 5) if word & 0x7E000000 == 0x36000000 else None


def string_references(image: bytes, text: bytes) -> list[int]:
    """Byte offsets of the ADRP that starts an ADRP+ADD forming the address of `text`'s first occurrence."""
    target = image.find(text)
    if target < 0:
        raise ValueError(f'{text!r} not in the image')
    found = []
    for i, word in enumerate(w := words(image)[:-1]):
        if word & 0x9F000000 != 0x90000000:
            continue
        low, high = (word >> 29) & 3, (word >> 5) & 0x7FFFF
        page = ((i * 4) & ~0xFFF) + (_offset((high << 2) | low, 21) << 12)
        follow = w[i + 1]
        if (
            follow & 0xFFC00000 == 0x91000000
            and (follow >> 5) & 31 == word & 31
            and page + ((follow >> 10) & 0xFFF) == target
        ):
            found.append(i * 4)
    return found


def _calls_before(image_words: tuple[int, ...], at: int, count: int) -> list[int]:
    """Targets of the `count` nearest BL instructions before byte offset `at`, nearest first."""
    out = []
    for i in range(at // 4 - 1, 0, -1):
        if image_words[i] >> 26 == 0b100101:
            out.append(branch_target(i * 4, image_words[i]))
            if len(out) == count:
                return out
    raise ValueError('not enough calls before the reference')


def _has_slack_constant(image_words: tuple[int, ...], call: int) -> bool:
    """Whether the function holds `mov wN, #0x104f`: 5 spare reserve entries (80) plus a page less one (4095)."""
    start = function_start(image_words, call) // 4
    return any(w & 0xFF800000 == 0x52800000 and (w >> 5) & 0xFFFF == 0x104F for w in image_words[start : call // 4])


def callers(image_words: tuple[int, ...], target: int) -> list[int]:
    return [
        i * 4
        for i, w in enumerate(image_words)
        if w >> 26 in (0b000101, 0b100101) and branch_target(i * 4, w) == target
    ]


def function_start(image_words: tuple[int, ...], at: int) -> int:
    i = at // 4
    while image_words[i] & 0xFFC07FFF != PROLOGUE:
        i -= 1
    return i * 4


def analyse(image: bytes) -> dict:
    """Find the add and delete functions, every call to add, and the shrink function's join and call to add."""
    w = words(image)
    handler_reference = string_references(image, ADD_ERROR)[0]
    add = _calls_before(w, handler_reference, 2)[1]
    delete = _calls_before(w, string_references(image, DELETE_ERROR)[0], 2)[1]
    add_calls = callers(w, add)
    handler = function_start(w, handler_reference)
    shrink_calls = [c for c in add_calls if function_start(w, c) != handler and _has_slack_constant(w, c)]
    (call,) = shrink_calls
    start = function_start(w, call)
    forward = [t for i in range(start // 4, call // 4) if (t := conditional_target(i * 4, w[i])) and start < t <= call]
    join = max(forward)
    return {
        'add': add,
        'delete': delete,
        'add_callers': add_calls,
        'shrink_start': start,
        'shrink_add_call': call,
        'shrink_join': join,
        'conditional_between_join_and_add': [i * 4 for i in range(join // 4, call // 4) if is_conditional(w[i])],
        'shrink_callers': callers(w, start - 4) + callers(w, start),
    }
