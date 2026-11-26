"""The boot args the packager builds against what start.s accepts before it draws anything.

start.s reads the boot args with the MMU off and enables early marks only for an exact geometry. This test evaluates that
gate on the values tools/package produces, so a change on either side shows here. Nothing is built.
"""

import re
import struct
import unittest
from pathlib import Path

from loader_abi import KERNEL_PHYS_BASE, NATIVE_FULL_RAM_EXCLUSIONS, VIDEO

ROOT = Path('.')
START = (ROOT / 'kernel/xnu-10063.141.1/osfmk/arm64/start.s').read_text()
LAYOUT = (ROOT / 'kernel/switch/include/switch/boot_layout.h').read_text()
MEMORY = (ROOT / 'boot/diag/erista-memory-bdinfo.log').read_text()

FIELD = {'virt': 0x8, 'phys': 0x10, 'size': 0x18, 'top': 0x20, 'base': 0x28, 'display': 0x30, 'stride': 0x38,
         'width': 0x40, 'height': 0x48, 'depth': 0x50}
BOOT_ARGS_FORMAT = '<HHI4Q6Q'
FIRST_MARK_ROW = 0xF5A00000 + (1279 - 16) * 2880
FRAMEBUFFER_SIZE = 2880 * 1280


def define(name):
    return int(re.search(rf'#define\s+{name}\s+(0x[0-9a-fA-F]+)', LAYOUT).group(1), 16)


def start_gate_source():
    begin = START.index('LEXT(start_first_cpu)')
    return START[begin : START.index('Lswitch_early_disabled:', begin)]


def packed_args(virt, phys, size, video):
    return struct.pack(BOOT_ARGS_FORMAT, 2, 2, 0, virt, phys, size, 0, *video)


def field(args, name):
    return struct.unpack_from('<Q', args, FIELD[name])[0]


def early_enabled(args, virt_base):
    """The branches of start_first_cpu's gate, in the order start.s takes them (the SCTLR test is a live register)."""
    phys, size, virt = (field(args, k) for k in ('phys', 'size', 'virt'))
    if phys == define('SWITCH_KERNEL_PHYS_BASE'):
        if size != 0x30000000 or virt != virt_base:
            return False
    elif phys != define('SWITCH_BOOT_PHYS_BASE') or size != define('SWITCH_BOOT_MEM_SIZE'):
        return False
    elif virt != virt_base - define('SWITCH_KERNEL_PHYS_BASE') + define('SWITCH_BOOT_PHYS_BASE'):
        return False
    if (field(args, 'base'), field(args, 'stride'), field(args, 'width'), field(args, 'height')) != (
        0xF5A00000, 2880, 720, 1280,
    ):
        return False
    display, depth = field(args, 'display'), field(args, 'depth')
    return (display == 1 and depth == 32) or (display == 0 and depth == 0x320)


class BootArgsContractTest(unittest.TestCase):
    def test_offsets_match_the_packers_struct(self):
        probe = packed_args(1, 2, 3, [4, 5, 6, 7, 8, 9])
        for name, value in zip(('virt', 'phys', 'size'), (1, 2, 3)):
            self.assertEqual(field(probe, name), value)
        for name, value in zip(('base', 'display', 'stride', 'width', 'height', 'depth'), (4, 5, 6, 7, 8, 9)):
            self.assertEqual(field(probe, name), value)
        gate = start_gate_source()
        for offset in ('0x28', '0x30', '0x38', '0x40', '0x48', '0x50'):
            self.assertIn(f'[x20, #{offset}]', gate)

    def test_kernel_base_agrees(self):
        self.assertEqual(KERNEL_PHYS_BASE, define('SWITCH_KERNEL_PHYS_BASE'))
        self.assertEqual(define('SWITCH_BOOT_PHYS_BASE'), 0x80000000)
        self.assertEqual(define('SWITCH_BOOT_MEM_SIZE'), 4 << 30)

    def test_memory_matches_the_measured_erista_banks(self):
        banks = {}
        for number in (0, 1):
            start = int(re.search(rf'DRAM bank {number} start = (0x[0-9A-Fa-f]+)', MEMORY).group(1), 16)
            size = int(re.search(rf'DRAM bank {number} size = (0x[0-9A-Fa-f]+)', MEMORY).group(1), 16)
            banks[number] = (start, size)
        self.assertEqual(banks[0][0], define('SWITCH_BOOT_PHYS_BASE'))
        self.assertEqual(banks[0][0] + banks[0][1], 0x100000000 - (3 << 20))
        self.assertEqual(banks[1], (0x100000000, 0x80000000))
        self.assertEqual(sum(size for _, size in banks.values()), (4 << 30) - (3 << 20))

    def test_framebuffer_sits_in_bank_zero_and_is_reserved(self):
        reserved = {name: (start, size) for name, start, size in NATIVE_FULL_RAM_EXCLUSIONS}['framebuffer']
        self.assertEqual(reserved[0], VIDEO[0])
        self.assertGreaterEqual(reserved[1], VIDEO[2] * VIDEO[4])
        bank0_end = 0x100000000 - (3 << 20)
        self.assertLessEqual(reserved[0] + reserved[1], bank0_end)

    def test_marks_stay_inside_the_framebuffer(self):
        """Marks are drawn in the logical 1280x720 landscape frame over the rotated 720x1280 panel: logical x selects the
        panel row from the bottom, logical y the column."""
        first, last = 1, 21
        for stage in (first, last):
            x0 = 16 + 40 * (stage - 1)
            self.assertLessEqual(x0 + 16, 1280)
            for logical_x in (x0, x0 + 15):
                for logical_y in (280, 295):
                    address = 0xF5A00000 + (1279 - logical_x) * 2880 + logical_y * 4
                    self.assertGreaterEqual(address, 0xF5A00000)
                    self.assertLessEqual(address + 4, 0xF5A00000 + FRAMEBUFFER_SIZE)
        self.assertLess(295 * 4 + 4, 2880)
        self.assertEqual(FIRST_MARK_ROW, 0xF5A00000 + 1263 * 2880)

    def test_mark_macro_clobbers_only_scratch_registers(self):
        begin = START.index('.macro SWITCH_EARLY_MARK')
        macro = START[begin : START.index('.endmacro', begin)]
        used = set(re.findall(r'\bx(\d+)\b', macro))
        self.assertEqual(used, {'16', '17', '18', '21', '28'})
        for stored in re.findall(r'\b(?:mov|MOV64|adrp|add|sub|subs)\s+x(\d+)', macro):
            self.assertIn(stored, {'16', '17', '18'})
        self.assertNotIn('bl ', macro)
        self.assertNotIn('sp', re.findall(r'\b(sp)\b', macro))

    def test_marks_before_the_mmu_use_physical_addresses_after_it_the_identity_block(self):
        self.assertIn('0xf5a00000 | ARM_TTE_VALID', START)
        self.assertIn('CACHE_ATTRINDX_DISABLE', START[START.index('0xf5a00000 | ARM_TTE_VALID'):][:300])

    def test_packaged_video_against_the_start_gate(self):
        """Recorded finding, not an endorsement. The packager's Video is display 1 with depth 0x320; start.s accepts
        0x320 only for display 0 and 32 only for display 1, so early rendering stays off on a package built from it.
        Changing either side must also change ERISTA_FIRST_XNU_BOOT.md."""
        virt = 0xFFFFFFEFD2200000
        full = packed_args(virt - define('SWITCH_KERNEL_PHYS_BASE') + define('SWITCH_BOOT_PHYS_BASE'),
                           define('SWITCH_BOOT_PHYS_BASE'), define('SWITCH_BOOT_MEM_SIZE'), VIDEO)
        self.assertEqual(VIDEO[1], 1)
        self.assertEqual(VIDEO[5], 0x320)
        self.assertFalse(early_enabled(full, virt))
        for video in (VIDEO[:1] + [1] + VIDEO[2:5] + [32], VIDEO[:1] + [0] + VIDEO[2:]):
            args = packed_args(field(full, 'virt'), field(full, 'phys'), field(full, 'size'), video)
            self.assertTrue(early_enabled(args, virt))


if __name__ == '__main__':
    unittest.main()
