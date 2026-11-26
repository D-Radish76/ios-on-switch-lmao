"""The D0 loader diagnostic: what its script may run, what its image holds, what its binary may contain.

The commands are listed here on their own, not derived from the generator, so a change to the list is a change to this
file as well. The disassembly checks run over synthetic text; a real link is inspected when a clang that targets
AArch64 and ld.lld are on PATH (Bazel's build action inspects every link regardless).
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from typing import ClassVar

from bl33 import bl33_patch
from boot_script import boards
from diag_boot import DWELL, carveout_table, diag_env_edits
from diag_loader import INI, board_gate, build, check_destination, diag_loader_bl33, diag_script, entry_ini
from diag_loader_inspect import FORBIDDEN_SYMBOLS, SPECIAL, WRITES, inspect_disassembly, inspect_map
from fdt_tree import tree
from loader_abi import (
    BL33_BASE,
    BOOT_ARGS_PAGE,
    DTB_SLOT,
    EXPORTER_SLOT,
    KERNEL_PHYS_BASE,
    LOADER_SLOT,
    SCRIPT_SLOT,
    SD_REFERENCE,
    STAGE_END,
    STAGE_HEADER,
    WINDOW_BASE,
    WINDOW_SIZE,
)

ROOT = Path('.')
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
LOADER_C = (ROOT / 'boot/loader/loader.c').read_text()
CARVEOUT_H = (ROOT / 'boot/loader/carveout.h').read_text()
TEMPLATE = (ROOT / 'boot/diag/loader/u-boot-script.txt').read_text()
BL33 = (ROOT / 'bazel/pinned/firmware/bl33.bin').read_bytes()
DTB = (ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes()
# The *_access.c files hold the raw accesses of T1, T1-R, T1 v3, T1-T, T2 and D0-S v2, each with its own tests.
SOURCES = sorted(
    p
    for p in (ROOT / 'boot/diag/loader').glob('*.[chS]')
    if p.name not in ('t1_access.c', 't1r_access.c', 't1v3_access.c', 't1t_access.c', 't2_access.c', 's2_access.c', 't4r_access.c')
)

COMMANDS = {'setenv', 'test', 'echoe', 'base', 'itest.l', 'fdt', 'cp.b', 'bootm', 'sleep', 'reset', 'true'}
STRUCTURE = {'if', 'then', 'else', 'fi', 'while', 'do', 'done'}
FDT_SUBCOMMANDS = {'addr', 'get', 'rsvmem'}
BOOTM_SUBCOMMANDS = {'start', 'loados', 'prep', 'go'}


def script(board: str = 'erista', dwell: int = DWELL) -> str:
    return diag_script(TEMPLATE, HEADER, board, len(DTB), dwell)


def commands(text: str) -> list[list[str]]:
    found = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(('#', 'for ')) or line in STRUCTURE:
            continue
        line = re.sub(r'^(if|elif|while)\s+', '', line)
        line = re.sub(r';\s*(?:then|do)$', '', line)
        for part in re.split(r'\s*(?:&&|\|\|)\s*', line):
            simple = re.sub(r'^!\s*', '', part.strip())
            if simple and simple != 'else':
                found.append(simple.split())
    return found


class Script(unittest.TestCase):
    def test_only_the_listed_commands_run(self) -> None:
        for command in commands(script()):
            self.assertIn(command[0], COMMANDS, command)
            if command[0] == 'fdt':
                self.assertIn(command[1], FDT_SUBCOMMANDS, command)
            if command[0] == 'bootm':
                self.assertIn(command[1], BOOTM_SUBCOMMANDS, command)

    def test_nothing_reads_the_card_or_writes_flash_or_loads_a_kernel(self) -> None:
        text = '\n'.join(line for line in script().splitlines() if not line.lstrip().startswith('#'))
        for forbidden in (
            'fatload',
            'fatwrite',
            'load mmc',
            'mmc write',
            'saveenv',
            'bootargs.txt',
            'exporter',
            'extlinux',
        ):
            self.assertNotIn(forbidden, text)
        self.assertNotIn('0xAA100000', text, 'the staged window is not checked: there is none')
        self.assertNotRegex(text, r'\bgo\s+0x')
        self.assertNotRegex(text, r'\bmw(\.[bwlq])?\b')

    def test_bootm_runs_in_order_once_and_only_when_ready(self) -> None:
        text = script()
        order = [c[1] for c in commands(text) if c[0] == 'bootm']
        self.assertEqual(['start', 'loados', 'prep', 'go'], order)
        go = text.index('bootm go')
        self.assertIn('if test "${handoff_status}" = "READY"; then', text[text.rindex('if test', 0, go) :])
        self.assertLess(text.index('bootm prep'), go)

    def test_the_loader_runs_from_its_slot_and_the_dtb_comes_from_its_slot(self) -> None:
        text = script()
        self.assertIn(f'bootm start {LOADER_SLOT[0]:#x} - 0x8D000000', text)
        self.assertIn(f'cp.b {DTB_SLOT[0]:#x} 0x8D000000 {len(DTB):#x}', text)

    def test_the_board_gate_accepts_the_named_row_alone(self) -> None:
        erista = script('erista')
        self.assertIn('"${t210b01}" = "0" -a "${sku}" = "0"', erista)
        self.assertNotIn('"${t210b01}" = "1"', erista)
        oled = script('oled')
        self.assertIn('"${t210b01}" = "1" -a "${sku}" = "3"', oled)
        with self.assertRaises(SystemExit):
            board_gate(HEADER, 'switch')

    def test_the_gate_does_not_read_or_change_the_boot_flag(self) -> None:
        self.assertEqual(0, next(boot for name, _, _, boot in boards(HEADER) if name == 'erista'))
        self.assertNotIn('boot', board_gate(HEADER, 'erista').lower().replace('setenv handoff_status', ''))

    def test_a_refusal_waits_then_resets_once_and_never_boots(self) -> None:
        text = script()
        tail = text[text.index('echoe "REFUSED') :]
        self.assertEqual(1, len(re.findall(r'^reset$', tail, re.MULTILINE)))
        self.assertLess(tail.index('sleep 5'), tail.index('\nreset\n'))
        self.assertNotIn('bootm', tail)
        self.assertEqual(1, text.count('\nreset\n'))
        self.assertTrue(tail.rstrip().endswith('done'))

    def test_the_wait_is_a_multiple_of_five_from_ten_to_sixty(self) -> None:
        self.assertIn('for left in 10 5 0; do', script(dwell=15))
        for bad in (7, 5, 65):
            with self.assertRaises(SystemExit):
                script(dwell=bad)

    def test_no_placeholder_is_left(self) -> None:
        self.assertNotRegex(script(), r'@[A-Z_0-9]+@')


class Image(unittest.TestCase):
    loader = b'\x27\x05\x19\x56' + bytes(60) + b'L' * 1000

    def image(self) -> bytes:
        return diag_loader_bl33(bl33_patch(BL33, diag_env_edits(DWELL)), script(), self.loader, DTB)

    def test_the_slots_hold_their_bytes_and_nothing_else_is_added(self) -> None:
        image = self.image()
        for (address, _), content in ((LOADER_SLOT, self.loader), (DTB_SLOT, DTB)):
            self.assertEqual(content, image[address - BL33_BASE : address - BL33_BASE + len(content)])
        self.assertEqual(b'\0', image[EXPORTER_SLOT[0] - BL33_BASE : EXPORTER_SLOT[0] - BL33_BASE + 1])
        self.assertEqual(DTB_SLOT[0] - BL33_BASE + len(DTB), len(image))
        self.assertLess(len(image), STAGE_HEADER - BL33_BASE)

    def test_there_is_no_exporter_and_no_kernel_window(self) -> None:
        image = self.image()
        exporter = (ROOT / 'bazel/pinned/firmware/exporter.uimg').read_bytes()
        self.assertNotIn(exporter[:64], image)
        self.assertEqual(bytes(EXPORTER_SLOT[1]), image[EXPORTER_SLOT[0] - BL33_BASE :][: EXPORTER_SLOT[1]])
        self.assertLessEqual(len(image), STAGE_HEADER - BL33_BASE)
        self.assertNotIn(b'SWCH', image[0x100000 - 0x1000 :])

    def test_u_boot_differs_from_the_pinned_one_only_by_its_environment_and_falls_back_to_a_wait(self) -> None:
        image = self.image()
        patched = bl33_patch(BL33, diag_env_edits(DWELL))
        self.assertEqual(patched, image[: len(BL33)])
        self.assertIn(b'DIAGNOSTIC DID NOT RUN', patched)
        self.assertIn(b'scan_dev_for_extlinux=true', patched)
        self.assertIn(b'scan_dev_for_scripts=source %#x' % SCRIPT_SLOT[0], patched)

    def test_a_slot_that_is_too_small_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            diag_loader_bl33(bl33_patch(BL33, diag_env_edits(DWELL)), script(), bytes(LOADER_SLOT[1] + 1), DTB)
        with self.assertRaises(SystemExit):
            diag_loader_bl33(bl33_patch(BL33, diag_env_edits(DWELL)), script(), self.loader, bytes(DTB_SLOT[1] + 1))

    def test_the_reservation_variant_is_its_own_entry_and_the_d0_ini_is_unchanged(self) -> None:
        d0 = (
            '[iOS Diag Loader]\nl4t=1\nboot_prefixes=/switchroot/ios-diag-loader/\nid=iDiagL\n'
            'icon=bootloader/res/icon_ios_hue.bmp\nbootwait=0\nbootcmd_mmc0=true\nscan_dev_for_extlinux=true\n'
        )
        self.assertEqual(INI, d0)
        variant = entry_ini('iOS Diag Loader R', 'ios-diag-loader-r', 'iDiagLR')
        renamed = d0.replace('iOS Diag Loader', 'iOS Diag Loader R').replace('ios-diag-loader', 'ios-diag-loader-r')
        self.assertEqual(variant, renamed.replace('iDiagL', 'iDiagLR'))

    def test_the_entry_files_are_the_diagnostics_own(self) -> None:
        with tempfile.TemporaryDirectory() as out:
            build(
                Namespace(
                    template=ROOT / 'boot/diag/loader/u-boot-script.txt',
                    boards=ROOT / 'kits/SwitchBoard/SwitchBoard.h',
                    board='erista',
                    loader=ROOT / 'bazel/pinned/firmware/bl31.bin',
                    dtb=ROOT / 'bazel/pinned/firmware/erista-sku0.dtb',
                    bl31=ROOT / 'bazel/pinned/firmware/bl31.bin',
                    bl33=ROOT / 'bazel/pinned/firmware/bl33.bin',
                    out=Path(out),
                    dwell=DWELL,
                    exporter=None,
                    sums=None,
                    entry='ios-diag-loader',
                    title='iOS Diag Loader',
                    ident='iDiagL',
                )
            )
            files = sorted(str(p.relative_to(out)) for p in Path(out).rglob('*') if p.is_file())
        self.assertEqual(
            [
                'bootloader/ini/ios-diag-loader.ini',
                'switchroot/ios-diag-loader/bl31.bin',
                'switchroot/ios-diag-loader/bl33.bin',
                'switchroot/ios-diag-loader/script.txt',
            ],
            files,
        )
        self.assertIn('boot_prefixes=/switchroot/ios-diag-loader/', INI)
        self.assertNotIn('switchroot/ios/', INI)
        self.assertIn('bootwait=0', INI)


class Destination(unittest.TestCase):
    def test_the_reviewed_sizes_pass(self) -> None:
        check_destination(86544, 954622)

    def test_a_loader_that_grows_into_something_the_package_places_is_refused(self) -> None:
        for size in (0x1F000, BL33_BASE - 0xA0000000 + 1, 0x4A000000):
            with self.assertRaises(SystemExit):
                check_destination(size, 954622)


class DestinationFromTheDtb(unittest.TestCase):
    def test_the_pinned_dtb_leaves_the_loader_alone(self) -> None:
        check_destination(86544, 954622, DTB)

    def test_a_reservation_over_the_loader_is_refused(self) -> None:
        for start, size in ((0xA0000000, 0x1000), (0x9FFFF000, 0x2000), (0xA0010000, 0x100000)):
            with self.assertRaises(SystemExit) as caught:
                check_destination(86544, 954622, tree([], {'x': (start, size)}))
            self.assertIn('DTB reservation', str(caught.exception))
        with self.assertRaises(SystemExit):
            check_destination(86544, 954622, tree([(0xA0000000, 0x10)], {}))

    def test_a_reservation_just_outside_is_allowed(self) -> None:
        check_destination(86544, 954622, tree([(0x9FFFF000, 0x1000)], {'x': (0xA0016000, 0x1000)}))

    def test_an_unreadable_dtb_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            check_destination(86544, 954622, DTB[:20])

    def test_build_passes_the_dtb_it_copies(self) -> None:
        source = (ROOT / 'tools/package/diag_loader.py').read_text()
        self.assertIn('check_destination(len(loader), len(image), dtb)', source)


class Console(unittest.TestCase):
    def test_progress_stays_on_the_screen(self) -> None:
        text = script()
        self.assertNotIn('serial', text)
        self.assertIn('setenv stdout vidconsole && setenv stderr vidconsole', text)
        for step in ('BOARD OK', 'FRAMEBUFFER OK', 'DTB COPIED', 'LOADER WRITTEN', 'HANDOFF'):
            self.assertIn(step, text)
        self.assertLess(text.index('BOARD OK'), text.index('FRAMEBUFFER OK'))
        self.assertLess(text.index('LOADER WRITTEN'), text.index('HANDOFF'))

    def test_a_console_that_cannot_be_set_runs_nothing_else(self) -> None:
        text = script()
        self.assertIn('setenv handoff_status CONSOLE_FAILED', text)
        self.assertIn('if test "${handoff_status}" = "WRONG_BOARD"; then', text)


class Tables(unittest.TestCase):
    def test_the_mc_table_is_native_gates(self) -> None:
        header = (ROOT / 'boot/diag/loader/mc_table.h').read_text()
        pairs = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+|\d+)\}', header.split('switch_diag_gic')[0])
        self.assertEqual(carveout_table(CARVEOUT_H), [(int(a, 16), int(v, 0)) for a, v in pairs])

    def test_the_gic_words_are_native_gates(self) -> None:
        gate = re.search(r'const uint32_t addresses\[\] = \{([^}]*)\}', LOADER_C)
        mine = re.search(r'switch_diag_gic\[\] = \{([^}]*)\}', (ROOT / 'boot/diag/loader/mc_table.h').read_text())
        assert gate is not None
        assert mine is not None
        self.assertEqual(re.findall(r'0x[0-9a-f]+', gate.group(1)), re.findall(r'0x[0-9a-f]+', mine.group(1)))

    def test_the_ranges_are_the_loaders(self) -> None:
        header = (ROOT / 'boot/diag/loader/ranges.h').read_text()

        def value(name: str) -> int:
            return int(re.search(rf'SWITCH_DIAG_{name}\s+UINT64_C\((0x[0-9a-f]+)\)', header).group(1), 16)  # type: ignore[union-attr]

        self.assertEqual(SD_REFERENCE, value('SD_REFERENCE'))
        self.assertEqual(BOOT_ARGS_PAGE, value('BOOT_ARGS'))
        self.assertEqual(STAGE_HEADER, value('STAGE_START'))
        self.assertEqual(STAGE_END, value('STAGE_END'))
        self.assertEqual(WINDOW_BASE, value('KERNEL_START'))
        self.assertEqual(WINDOW_BASE + WINDOW_SIZE, value('KERNEL_END'))
        self.assertLess(WINDOW_BASE, KERNEL_PHYS_BASE)

    def test_the_checked_constants_are_the_scripts(self) -> None:
        checks = (ROOT / 'boot/diag/loader/checks.h').read_text()
        self.assertRegex(checks, r'SWITCH_DIAG_FB_PA\s+UINT64_C\(0xf5a00000\)')
        self.assertRegex(checks, r'SWITCH_DIAG_FDT_PA\s+UINT64_C\(0x8d000000\)')
        self.assertIn('0xF5A00000', TEMPLATE)
        self.assertIn('0x8D000000', TEMPLATE)


class Readme(unittest.TestCase):
    text = (ROOT / 'boot/diag/loader/README.md').read_text()

    def test_every_register_written_is_in_the_table(self) -> None:
        for register in WRITES:
            self.assertIn(f'`{register}`', self.text, register)


class Sources(unittest.TestCase):
    def test_nothing_of_the_kernel_path_is_included(self) -> None:
        for path in SOURCES:
            for include in re.findall(r'#include\s+"([^"]+)"', path.read_text()):
                self.assertNotIn(
                    Path(include).name,
                    {
                        'layout.h',
                        'runs.h',
                        'copy.h',
                        'emc.h',
                        'seed.h',
                        'boot_args.h',
                        'stage.h',
                        'stamps.h',
                        'sd_reference.h',
                    },
                    f'{path}: {include}',
                )
            self.assertNotIn('loader_enter_kernel', path.read_text(), path)

    def test_no_store_goes_through_a_volatile_cast(self) -> None:
        for path in SOURCES:
            self.assertNotRegex(
                path.read_text(), r'\*\s*\(\s*(?:const\s+)?volatile[^;=]*\)[^;=]*(?<![=!<>])=(?!=)', path
            )

    def test_a_device_address_is_formed_only_in_read32(self) -> None:
        sites = [
            (path.name, line)
            for path in SOURCES
            for line in path.read_text().splitlines()
            if '(uintptr_t)' in line and 'volatile' in line
        ]
        self.assertEqual([], [s for s in sites if s[0] != 'diag.c'])
        self.assertEqual(
            [
                '    return *(const volatile uint32_t *)(uintptr_t)address;',
                '        const volatile uint32_t *fdt = (const volatile uint32_t *)(uintptr_t)pointer;',
            ],
            [line for name, line in sites if name == 'diag.c'],
        )

    def test_the_mc_block_is_never_a_store_target_in_the_sources(self) -> None:
        for path in SOURCES:
            if path.name in {'mc_table.h', 'ranges.h'}:
                continue
            self.assertNotIn('0x7001', path.read_text(), path)


def disassembly(extra: str = '', without: tuple[str, ...] = ()) -> str:
    lines = ['00000000a0000000 <_start>:']
    for target, (_, count) in WRITES.items():
        if target in without:
            continue
        lines += [f'a0000000: msr {target}, x0'] * count
    for mnemonic, count in SPECIAL.items():
        if mnemonic in without:
            continue
        lines += [f'a0000000: {mnemonic} #0'] * count
    lines += extra.splitlines()
    lines += [
        '00000000a000023c <switch_diag_read32>:',
        'a000023c: mov w8, w0',
        'a0000240: ldr w0, [x8]',
        'a0000244: ret',
    ]
    return '\n'.join(lines) + '\n'


class Inspection(unittest.TestCase):
    def test_the_reviewed_binary_passes(self) -> None:
        self.assertEqual([], inspect_disassembly(disassembly()))

    def test_an_unreviewed_system_register_write_fails(self) -> None:
        problems = inspect_disassembly(disassembly('a0000100: msr SCTLR_EL2, x0\n'))
        self.assertTrue(any('SCTLR_EL2' in p for p in problems), problems)

    def test_a_second_write_of_a_listed_register_fails(self) -> None:
        problems = inspect_disassembly(disassembly('a0000100: msr HCR_EL2, x0\n'))
        self.assertTrue(any('HCR_EL2' in p for p in problems), problems)

    def test_a_listed_write_that_vanished_fails(self) -> None:
        self.assertTrue(inspect_disassembly(disassembly(without=('CNTKCTL_EL1',))))

    def test_forbidden_instructions_fail(self) -> None:
        for instruction in ('hvc #0', 'svc #0', 'ic iallu', 'dc civac, x0', 'wfi', 'brk #0'):
            self.assertTrue(inspect_disassembly(disassembly(f'a0000100: {instruction}\n')), instruction)

    def test_a_second_smc_or_eret_fails_and_a_missing_one_fails(self) -> None:
        self.assertTrue(inspect_disassembly(disassembly('a0000100: smc #0\n')))
        self.assertTrue(inspect_disassembly(disassembly('a0000100: eret\n')))
        self.assertTrue(inspect_disassembly(disassembly(without=('smc',))))

    def test_a_device_address_built_in_code_fails_except_the_identity_registers(self) -> None:
        self.assertEqual([], inspect_disassembly(disassembly('a0000100: movk w8, #0x7000, lsl #16\n')))
        for window in (0x7001, 0x6000, 0x5004, 0x7FFF):
            problems = inspect_disassembly(disassembly(f'a0000100: movk w8, #{window:#x}, lsl #16\n'))
            self.assertTrue(any('device address' in p for p in problems), (window, problems))
        self.assertEqual([], inspect_disassembly(disassembly('a0000100: movk x8, #0xf5a0, lsl #16\n')))

    def test_read32_must_be_one_load_and_a_return(self) -> None:
        stored = disassembly().replace('ldr w0, [x8]', 'ldr w0, [x8]\na0000241: str w0, [x8]')
        self.assertTrue(inspect_disassembly(stored))
        self.assertTrue(inspect_disassembly(disassembly().replace('ldr w0, [x8]', 'ldr x0, [x8]')))
        self.assertTrue(inspect_disassembly(disassembly().replace('switch_diag_read32', 'something')))

    def test_the_map_must_not_carry_the_kernel_path(self) -> None:
        names = ' '.join(
            ['switch_diag_read32', 'switch_diag_exit', 'switch_diag_after_transition', 'probe_show_exception']
        )
        self.assertEqual([], inspect_map(names))
        for symbol in FORBIDDEN_SYMBOLS:
            self.assertTrue(inspect_map(names + ' ' + symbol), symbol)
        self.assertTrue(inspect_map('switch_diag_read32'))


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump'), 'no LLVM on PATH'
)
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [
        '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
        '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-O2', '-Wall', '-Wextra', '-Werror', '-DPROBE_QUIET=1',
    ]  # fmt: skip
    INCLUDES: ClassVar[list[str]] = ['boot/diag/loader', 'boot/handoff', 'boot/loader', 'kits/SwitchBoard']

    def test_it_compiles_links_and_passes_the_inspection(self) -> None:
        sources = [f'boot/diag/loader/{n}' for n in ('entry.S', 'diag.c', 'checks.c', 'report.c', 'exit.c')]
        sources.append('boot/loader/memory.c')
        with tempfile.TemporaryDirectory() as scratch:
            objects = []
            for i, source in enumerate(sources):
                obj = f'{scratch}/{i}.o'
                command = [
                    'clang',
                    *self.FLAGS,
                    *(f for inc in self.INCLUDES for f in ('-I', inc)),
                    '-c',
                    source,
                    '-o',
                    obj,
                ]
                result = subprocess.run(command, capture_output=True, text=True, check=False)
                if result.returncode and 'unable to create target' in result.stderr:
                    self.skipTest('this clang does not target AArch64')
                self.assertEqual(0, result.returncode, result.stderr)
                objects.append(obj)
            elf, linkmap = f'{scratch}/d0.elf', f'{scratch}/d0.map'
            linked = subprocess.run(
                ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
                capture_output=True, text=True, check=False,
            )  # fmt: skip
            self.assertEqual(0, linked.returncode, linked.stderr)
            dis = subprocess.run(
                ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
            )
            self.assertEqual([], inspect_map(Path(linkmap).read_text()))
            self.assertEqual([], inspect_disassembly(dis.stdout))


if __name__ == '__main__':
    unittest.main()
