"""The device-window audit: synthetic disassembly for each way an address can reach an access, and a real T1-T link."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import diag_images
from diag_mmio_audit import audit

LLVM = (
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump') and shutil.which('llvm-objcopy')
)


def function(name: str, *lines: str) -> str:
    body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
    return f'0000000000000100 <{name}>:\n{body}\n'


READ32 = function('switch_diag_read32', 'mov x8, x0', 'ldr w0, [x8]', 'ret')
T1T = function('switch_t1t_read32', 'mov x8, x0', 'ldr w0, [x8]', 'ret') + function(
    'switch_t1t_access', 'bl 0x100 <switch_t1t_read32>'
)
IDS = function(
    'switch_diag_after_transition',
    'mov w0, #0x804 // =2052',
    'movk w0, #0x7000, lsl #16',
    'bl 0x100 <switch_diag_read32>',
    'mov w0, #0xf9d8',
    'movk w0, #0x7000, lsl #16',
    'bl 0x100 <switch_diag_read32>',
)


def problems_of(*extra: str) -> list[str]:
    return audit(READ32 + IDS + ''.join(extra))[1]


class Synthetic(unittest.TestCase):
    def test_only_the_two_identity_reads_pass(self) -> None:
        self.assertEqual([], problems_of())

    def test_a_missing_identity_read_is_reported(self) -> None:
        self.assertTrue(audit(READ32)[1])

    def test_an_i2c_address_loaded_through_is_refused(self) -> None:
        extra = function('rogue', 'mov w8, #0xc004', 'movk w8, #0x7000, lsl #16', 'ldr w0, [x8]')
        self.assertTrue([p for p in problems_of(extra) if '0x7000c004' in p])

    def test_an_i2c_address_stored_through_is_refused(self) -> None:
        extra = function('rogue', 'mov w8, #0xc004', 'movk w8, #0x7000, lsl #16', 'str w1, [x8]')
        self.assertTrue(problems_of(extra))

    def test_an_address_passed_to_another_call_is_refused(self) -> None:
        extra = function('rogue', 'mov w0, #0xc004', 'movk w0, #0x7000, lsl #16', 'bl 0x100 <other>')
        self.assertTrue(problems_of(extra))

    def test_a_copy_that_reaches_an_access_is_followed(self) -> None:
        extra = function('rogue', 'mov w9, #0xc004', 'movk w9, #0x7000, lsl #16', 'mov x8, x9', 'str w1, [x8]')
        self.assertTrue(problems_of(extra))

    def test_an_address_plus_an_offset_is_followed(self) -> None:
        extra = function('rogue', 'mov w8, #0xc000', 'movk w8, #0x7000, lsl #16', 'add x9, x8, #0x94', 'ldr w0, [x9]')
        self.assertTrue([p for p in problems_of(extra) if '0x7000c094' in p])

    def test_a_cap_page_address_is_refused(self) -> None:
        extra = function('rogue', 'mov w8, #0x6010', 'movk w8, #0x6000, lsl #16', 'str w1, [x8]')
        self.assertTrue(problems_of(extra))

    def test_a_window_page_from_adrp_is_refused(self) -> None:
        extra = function('rogue', 'adrp x8, 0x7000c000 <x>', 'ldr w0, [x8, #4]')
        self.assertTrue([p for p in problems_of(extra) if 'adrp' in p])

    def test_a_ram_size_constant_that_is_only_compared_is_listed_and_accepted(self) -> None:
        extra = function('check', 'mov w8, #0x7fd00000', 'cmp x0, x8', 'ret')
        report, problems = audit(READ32 + IDS + extra)
        self.assertEqual([], problems)
        self.assertIn('0x7fd00000', '\n'.join(report))

    def test_a_second_caller_of_the_t1t_raw_read_is_refused(self) -> None:
        extra = function('rogue', 'bl 0x100 <switch_t1t_read32>')
        self.assertTrue([p for p in audit(READ32 + IDS + T1T + extra)[1] if 'rogue' in p])
        self.assertEqual([], audit(READ32 + IDS + T1T)[1])


@unittest.skipUnless(LLVM, 'no LLVM on PATH')
class RealImage(unittest.TestCase):
    def test_the_t1t_link_passes_the_audit(self) -> None:
        includes = [f'-I{diag_images.BOOT / d}' for d in diag_images.INCLUDES] + [f'-I{diag_images.BOARD_INCLUDE}']
        with tempfile.TemporaryDirectory() as scratch:
            objects = []
            for i, source in enumerate(diag_images.COMMON + diag_images.T1T + diag_images.TAIL):
                obj = f'{scratch}/{i}.o'
                command = ['clang', *diag_images.COPTS, '-DSWITCH_DIAG_T1T=1', *includes]
                result = subprocess.run(
                    [*command, '-c', str(diag_images.BOOT / source), '-o', obj],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if result.returncode and 'unable to create target' in result.stderr:
                    self.skipTest('this clang does not target AArch64')
                self.assertEqual(0, result.returncode, result.stderr)
                objects.append(obj)
            link = [
                'ld.lld', '-T', str(diag_images.BOOT / 'diag/loader/link.ld'), '-o', f'{scratch}/e.elf', *objects
            ]  # fmt: skip
            subprocess.run(link, check=True, capture_output=True)
            subprocess.run(['llvm-objcopy', '-O', 'binary', f'{scratch}/e.elf', f'{scratch}/f.bin'], check=True)
            dis = subprocess.run(
                ['llvm-objdump', '-d', '--no-show-raw-insn', f'{scratch}/e.elf'],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            report, problems = audit(dis, Path(f'{scratch}/f.bin').read_bytes(), t1t=True)
        self.assertEqual([], problems)
        text = '\n'.join(report)
        self.assertIn('0x70000804', text)
        self.assertIn('0x7000f9d8', text)
        self.assertNotIn('0x7000c0', text)
