"""The D1 loader diagnostic: its script's every path, run on a model, and what its package and inspection require.

The script is the real one, filled in by diag_loader.build from the pinned files and run on script_sim.py's model of
the few commands it uses. The model has no U-Boot, caches or card: it shows what the script does when a command
reports failure or RAM holds something else, not what the hardware does.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from typing import ClassVar

import script_sim
from diag_loader import build
from diag_loader_inspect import inspect_disassembly, inspect_map
from exporter_check import (
    BL33_PIN,
    EXPORTER_PIN,
    FLAGS_OFFSET,
    HELPERS,
    STATUS_BASE,
    STATUS_BYTES,
    STATUS_FLAGS,
    STATUS_MAGIC,
    STATUS_VERSION,
    check_exporter,
    check_pinned,
    crc_as_stored,
    helper_words,
)
from loader_abi import BL33_BASE, EXPORTER_RUN, EXPORTER_SLOT, SD_REFERENCE

ROOT = Path('.')
PINNED = ROOT / 'bazel/pinned'
SUMS = (PINNED / 'SHA256SUMS').read_text()
BL33 = (PINNED / 'firmware/bl33.bin').read_bytes()
EXPORTER = (PINNED / 'firmware/exporter.uimg').read_bytes()
TEMPLATE = ROOT / 'boot/diag/sd_loader/u-boot-script.txt'
EXPORTER_AT = EXPORTER_SLOT[0]
RUN = EXPORTER_RUN[0]
FIRST_GO = hex(RUN)
STATUS_MAGIC_AT = STATUS_BASE


def packaged(exporter: Path | None = None, out: Path | None = None) -> tuple[bytes, str]:
    """Build the entry as the Bazel rule does, with a stand-in loader; return its bl33.bin and script."""
    with tempfile.TemporaryDirectory() as scratch:
        target = out or Path(scratch)
        loader = Path(scratch) / 'loader.bin'
        loader.write_bytes(bytes(0x4000))
        build(
            Namespace(
                template=TEMPLATE,
                boards=ROOT / 'kits/SwitchBoard/SwitchBoard.h',
                board='erista',
                loader=loader,
                dtb=PINNED / 'firmware/erista-sku0.dtb',
                bl31=PINNED / 'firmware/bl31.bin',
                bl33=PINNED / 'firmware/bl33.bin',
                out=target,
                dwell=15,
                exporter=exporter or PINNED / 'firmware/exporter.uimg',
                sums=PINNED / 'SHA256SUMS',
                entry='ios-diag-loader-sd',
                title='iOS Diag Loader SD',
                ident='iDiagLS',
            )
        )
        root = target / 'switchroot/ios-diag-loader-sd'
        return (root / 'bl33.bin').read_bytes(), (root / 'script.txt').read_text()


IMAGE, SCRIPT = packaged()


def machine(**env: str) -> script_sim.Machine:
    """Return a model whose RAM holds what hekate loaded: the exporter in its slot and U-Boot's two cache helpers."""
    model = script_sim.Machine(env)
    slot = EXPORTER_AT - BL33_BASE
    model.write(EXPORTER_AT, IMAGE[slot : slot + len(EXPORTER)])
    first = HELPERS[0] - BL33_BASE
    model.write(HELPERS[0], BL33[first : first + 0x200])
    return model


def run(model: script_sim.Machine) -> str:
    return script_sim.run_script(model, SCRIPT)


def verbs(model: script_sim.Machine, name: str, *prefix: str) -> list[tuple]:
    return [e for e in model.events if e[0] == name and e[1 : 1 + len(prefix)] == prefix]


class Success(unittest.TestCase):
    def test_a_clean_run_hands_off_with_every_step_recorded(self) -> None:
        model = machine()
        self.assertEqual(run(model), 'handoff')
        self.assertEqual(model.env['handoff_status'], 'READY')
        self.assertEqual([model.word(STATUS_BASE + 0x20 + 4 * i) for i in range(len(STATUS_FLAGS))], [1] * 10)
        self.assertEqual(model.word(STATUS_MAGIC_AT), STATUS_MAGIC)

    def test_the_status_magic_is_written_last(self) -> None:
        model = machine()
        run(model)
        writes = [e for e in model.events if e[0] == 'mw.l' and e[1].startswith(hex(STATUS_BASE)[:7])]
        self.assertEqual(writes[-1][1], hex(STATUS_BASE))

    def test_the_card_is_read_once_and_the_steps_run_in_order(self) -> None:
        model = machine()
        run(model)
        self.assertEqual(verbs(model, 'read'), [('read', 'mmc', '1', '0xA001F200', '0', '1')])
        order = [e[:3] for e in model.events if e[0] in ('bootm', 'read', 'go')]
        self.assertEqual(order.index(('read', 'mmc', '1')) > order.index(('bootm', 'prep')), True)
        go_exporter = order.index(('go', FIRST_GO))
        self.assertGreater(go_exporter, order.index(('read', 'mmc', '1')))
        self.assertLess(go_exporter, order.index(('bootm', 'go')))

    def test_both_helpers_run_before_the_loader_is_loaded(self) -> None:
        model = machine()
        run(model)
        calls = [e for e in model.events if e[0] in ('go', 'bootm') and e[1] != 'go']
        helpers = [i for i, e in enumerate(model.events) if e[0] == 'go' and e[1] in (hex(h) for h in HELPERS)]
        loader_start = [i for i, e in enumerate(model.events) if e[:2] == ('bootm', 'start')][1]
        self.assertEqual(len(helpers), 2)
        self.assertTrue(all(i < loader_start for i in helpers))
        self.assertTrue(calls)

    def test_the_exporter_is_checked_by_bootm_with_verify_yes_and_the_loader_is_not(self) -> None:
        model = machine()
        run(model)
        verify = [(e[1], e[2]) for e in model.events if e[0] == 'setenv' and e[1] == 'verify']
        self.assertEqual([v[1] for v in verify], ['yes', 'no', 'no'])


# status, setup, exporter go expected (0, 1), reads expected, handoff never
FAILURES = {
    'WRONG_BOARD': 'board',
    'STATUS_BLOCK_FAILED': 'status',
    'SD_REFERENCE_WRONG_DEVICE': 'devnum',
    'EXPORTER_SLOT_HEADER_MISMATCH': 'size',
    'EXPORTER_SLOT_CRC_MISMATCH': 'slot data',
    'CACHE_HELPER_MISMATCH': 'helper word',
    'EXPORTER_HEADER_FAILED': 'bootm start',
    'EXPORTER_LOAD_FAILED': 'bootm loados',
    'EXPORTER_IMAGE_CORRUPT': 'run area after load',
    'CACHE_HELPER_RETURN_UNEXPECTED': 'helper ok',
    'SD_REFERENCE_CLEAR_FAILED': 'clear',
    'SD_REFERENCE_READ_FAILED': 'read',
    'EXPORTER_IMAGE_CHANGED': 'run area after read',
    'SD_CARD_EXPORT_FAILED': 'exporter rc',
    'SD_CARD_RECORD_FAILED': 'bad record',
    'SD_REFERENCE_METADATA_FAILED': 'metadata',
}


def flip(model: script_sim.Machine, address: int) -> None:
    model.write(address, bytes([model.read(address, 1)[0] ^ 0x01]))


def fail_on(model: script_sim.Machine, name: str, first: str | None = None) -> None:
    model.fail = lambda e: e[0] == name and (first is None or e[1].lower() == first)


def fail_loados_once(model: script_sim.Machine) -> None:
    seen: list[int] = []
    model.fail = lambda e: e[:2] == ('bootm', 'loados') and not seen.append(1) and len(seen) == 1


INJECTIONS = {
    'board': lambda m: m.env.update(t210b01='1'),
    'status': lambda m: fail_on(m, 'mw.b', hex(STATUS_BASE)),
    'devnum': lambda m: m.env.update(devnum='0'),
    'size': lambda m: flip(m, EXPORTER_AT + 12),
    'slot data': lambda m: flip(m, EXPORTER_AT + 64 + 100),
    'helper word': lambda m: flip(m, HELPERS[1] + 4),
    'bootm start': lambda m: fail_on(m, 'bootm', 'start'),
    'bootm loados': fail_loados_once,
    'run area after load': lambda m: setattr(m, 'after_loados', lambda n: n.write(RUN + 50, b'\xff')),
    'helper ok': lambda m: setattr(m, 'helper_rc', 0),
    'clear': lambda m: fail_on(m, 'mw.b', hex(SD_REFERENCE)),
    'read': lambda m: fail_on(m, 'read'),
    'run area after read': lambda m: setattr(m, 'before_read', lambda n: n.write(RUN + 60, b'\xee')),
    'exporter rc': lambda m: setattr(m, 'exporter_rc', 1),
    'bad record': lambda m: setattr(m, 'exporter_record', False),
    'metadata': lambda m: fail_on(m, 'crc32', hex(SD_REFERENCE + 0x200)),
}


def inject(case: str) -> script_sim.Machine:
    model = machine()
    INJECTIONS[case](model)
    return model


class Failures(unittest.TestCase):
    def test_each_failure_ends_in_reset_with_its_own_status(self) -> None:
        seen = {}
        for status, case in FAILURES.items():
            with self.subTest(status=status):
                model = inject(case)
                self.assertEqual(run(model), 'reset')
                self.assertEqual(model.env['handoff_status'], status)
                seen[status] = case
        self.assertEqual(len(set(seen)), len(FAILURES))

    def test_no_failure_reaches_the_loader_or_leaves_the_status_magic(self) -> None:
        for status, case in FAILURES.items():
            with self.subTest(status=status):
                model = inject(case)
                run(model)
                self.assertEqual(verbs(model, 'bootm', 'go'), [])
                self.assertNotIn(('HANDOFF',), model.events)
                self.assertEqual(model.word(STATUS_MAGIC_AT), 0)

    def test_the_exporter_does_not_run_unless_every_gate_before_it_held(self) -> None:
        after_go = {'SD_CARD_EXPORT_FAILED', 'SD_CARD_RECORD_FAILED', 'SD_REFERENCE_METADATA_FAILED'}
        for status, case in FAILURES.items():
            with self.subTest(status=status):
                model = inject(case)
                run(model)
                self.assertEqual(len(verbs(model, 'go', FIRST_GO)), 1 if status in after_go else 0)

    def test_the_card_is_read_at_most_once_and_never_after_a_failed_read(self) -> None:
        before_read = set(FAILURES) - {
            'EXPORTER_IMAGE_CHANGED',
            'SD_CARD_EXPORT_FAILED',
            'SD_CARD_RECORD_FAILED',
            'SD_REFERENCE_METADATA_FAILED',
            'SD_REFERENCE_READ_FAILED',
        }
        for status, case in FAILURES.items():
            with self.subTest(status=status):
                model = inject(case)
                run(model)
                reads = verbs(model, 'read')
                self.assertEqual(len(reads), 0 if status in before_read else 1)

    def test_the_status_is_shown_and_the_wait_precedes_the_reset(self) -> None:
        model = inject('exporter rc')
        run(model)
        echoed = [e for e in model.events if e[0] == 'echoe' and e[1].startswith('REFUSED')]
        self.assertIn('SD_CARD_EXPORT_FAILED', echoed[-1][1])
        self.assertEqual(model.events[-1][0], 'reset')

    def test_a_failed_status_block_clear_stops_before_anything_is_loaded(self) -> None:
        model = inject('status')
        run(model)
        self.assertEqual(verbs(model, 'bootm'), [])
        self.assertEqual(verbs(model, 'read'), [])

    def test_a_crc_change_in_any_one_place_is_caught_by_its_own_check(self) -> None:
        for case, status in (('slot data', 'EXPORTER_SLOT_CRC_MISMATCH'), ('size', 'EXPORTER_SLOT_HEADER_MISMATCH')):
            with self.subTest(case=case):
                model = inject(case)
                run(model)
                self.assertEqual(model.env['handoff_status'], status)
                self.assertEqual(verbs(model, 'bootm', 'start'), [])


LINES = [line.strip() for line in SCRIPT.splitlines() if line.strip() and not line.strip().startswith('#')]
ALLOWED = {
    *('setenv', 'test', 'echoe', 'base', 'itest.l', 'fdt', 'cp.b', 'bootm', 'sleep', 'reset', 'true'),
    *('mw.b', 'mw.l', 'crc32', 'go', 'read'),
    *('if', 'then', 'else', 'fi', 'for', 'while', 'do', 'done'),
}


class Static(unittest.TestCase):
    def words(self) -> list[list[str]]:
        found = []
        for line in LINES:
            for part in re.split(r'\s*(?:&&|;)\s*', line):
                tokens = part.split()
                while tokens and tokens[0] in ('if', 'then', 'else'):
                    tokens = tokens[1:]
                if tokens:
                    found.append(tokens)
        return found

    def test_no_placeholder_is_left(self) -> None:
        self.assertNotRegex(SCRIPT, r'@[A-Z_0-9]+@')

    def test_only_listed_commands_are_used(self) -> None:
        used = {tokens[0] for tokens in self.words() if tokens[0] not in ('fi', 'done', 'do')}
        self.assertEqual(used - ALLOWED, set())

    def test_nothing_saves_the_environment_or_touches_the_card_by_another_route(self) -> None:
        for forbidden in ('saveenv', 'mmc', 'fatload', 'load', 'run', 'source', 'md', 'mm', 'nm', 'env', 'ext4load'):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, {tokens[0] for tokens in self.words()})

    def test_exactly_one_read_and_it_is_sector_zero_of_mmc_1(self) -> None:
        reads = [t for t in self.words() if t[0] == 'read']
        self.assertEqual(reads, [['read', 'mmc', '1', '0xA001F200', '0', '1']])
        self.assertNotIn('mmc', {t[0] for t in self.words()})

    def test_go_only_targets_the_two_helpers_and_the_exporter(self) -> None:
        targets = sorted(t[1] for t in self.words() if t[0] == 'go')
        self.assertEqual(targets, sorted([hex(HELPERS[0]), hex(HELPERS[1]), hex(RUN)]))

    def test_there_is_one_bootm_go_and_nothing_follows_it_but_the_refusal(self) -> None:
        self.assertEqual(sum(1 for t in self.words() if t[:2] == ['bootm', 'go']), 1)
        after = SCRIPT.split('bootm go', 1)[1]
        self.assertIn('UNEXPECTED_BOOTM_RETURN', after.splitlines()[1])

    def test_memory_is_written_only_to_the_status_block_and_the_reference_page(self) -> None:
        allowed = ((STATUS_BASE, STATUS_BASE + STATUS_BYTES), (SD_REFERENCE, SD_REFERENCE + 0x1000))
        for tokens in self.words():
            if tokens[0] in ('mw.b', 'mw.l'):
                address = int(tokens[1], 0)
                self.assertTrue(any(lo <= address < hi for lo, hi in allowed), tokens)
        for tokens in self.words():
            if tokens[0] == 'crc32':
                destination = int(tokens[3], 0)
                self.assertTrue(any(lo <= destination < hi for lo, hi in allowed), tokens)

    def test_the_exporter_is_loaded_with_verify_yes_and_verify_is_no_again_before_the_loader(self) -> None:
        text = '\n'.join(LINES)
        self.assertLess(text.index('setenv verify yes'), text.index('bootm start ' + hex(EXPORTER_AT)))
        self.assertLess(text.index('bootm loados'), text.index('setenv verify no'))
        self.assertLess(text.index('setenv verify no'), text.index('bootm start 0xaa080000'))

    def test_every_check_value_comes_from_the_pinned_files(self) -> None:
        crc = hex(crc_as_stored(check_exporter(EXPORTER, SUMS).data_crc))
        self.assertEqual(SCRIPT.count(f'== {crc}'), 4)
        for address in HELPERS:
            for i, word in enumerate(helper_words(BL33, address)):
                self.assertIn(f'itest.l *{address + 4 * i:#x} == {word:#x}', SCRIPT)


class Package(unittest.TestCase):
    def test_the_exporter_is_in_its_slot_unchanged(self) -> None:
        at = EXPORTER_AT - BL33_BASE
        self.assertEqual(IMAGE[at : at + len(EXPORTER)], EXPORTER)

    def test_the_entry_is_independent_of_every_other_entry(self) -> None:
        with tempfile.TemporaryDirectory() as out:
            packaged(out=Path(out))
            files = sorted(str(p.relative_to(out)) for p in Path(out).rglob('*') if p.is_file())
            ini = (Path(out) / 'bootloader/ini/ios-diag-loader-sd.ini').read_text()
        self.assertEqual(
            files,
            [
                'bootloader/ini/ios-diag-loader-sd.ini',
                'switchroot/ios-diag-loader-sd/bl31.bin',
                'switchroot/ios-diag-loader-sd/bl33.bin',
                'switchroot/ios-diag-loader-sd/script.txt',
            ],
        )
        self.assertIn('[iOS Diag Loader SD]', ini)
        self.assertIn('boot_prefixes=/switchroot/ios-diag-loader-sd/', ini)
        for other in ('ios', 'ios-diag', 'ios-diag-loader', 'ios-diag-loader-r', 'ios-diag-memory', 'ios-diag-sd'):
            self.assertNotIn(f'/switchroot/{other}/', ini)

    def test_a_modified_exporter_is_refused_before_anything_is_built(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            changed = Path(scratch) / 'exporter.uimg'
            changed.write_bytes(EXPORTER[:-1] + bytes([EXPORTER[-1] ^ 1]))
            with self.assertRaises(SystemExit):
                packaged(exporter=changed)

    def test_the_pin_and_header_checks_each_refuse(self) -> None:
        with self.assertRaises(SystemExit):
            check_pinned(BL33[:-1] + b'\1', SUMS, BL33_PIN)
        with self.assertRaises(SystemExit):
            check_pinned(EXPORTER + b'\0', SUMS, EXPORTER_PIN)
        check_exporter(EXPORTER, SUMS)

    def test_the_header_is_refused_when_it_disagrees_with_the_run_area(self) -> None:
        for offset in (0, 4, 12, 16, 20, 24, 28, 29, 30, 31, 40):
            with self.subTest(offset=offset):
                bad = bytearray(EXPORTER)
                bad[offset] ^= 1
                forged = SUMS.replace(
                    __import__('exporter_check').pin(SUMS, EXPORTER_PIN),
                    __import__('hashlib').sha256(bad).hexdigest(),
                )
                with self.assertRaises(SystemExit):
                    check_exporter(bytes(bad), forged)

    def test_the_exporter_ends_inside_its_slot_and_run_area(self) -> None:
        self.assertLessEqual(len(EXPORTER), EXPORTER_SLOT[1])
        self.assertLessEqual(check_exporter(EXPORTER, SUMS).data_bytes, STATUS_BASE - RUN)

    def test_the_stored_crc_is_the_word_itest_reads_after_crc32(self) -> None:
        self.assertEqual(crc_as_stored(0x630D1CB8), 0xB81C0D63)


class Inspection(unittest.TestCase):
    def function(self, name: str, *lines: str) -> str:
        body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
        return f'0000000000000100 <{name}>:\n{body}\n'

    def test_the_map_needs_the_validator_in_d1_and_refuses_it_in_d0(self) -> None:
        text = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
        self.assertTrue(inspect_map(text, True))
        sd = text + ' switch_sd_card_reference_validate switch_diag_sd_evaluate'
        self.assertEqual(inspect_map(sd, True), [])
        self.assertTrue(inspect_map(sd, False))

    def test_the_controller_address_is_allowed_only_as_a_compare_in_the_validator(self) -> None:
        compare = self.function('validate_reference', 'mov w8, #0x700b0000', 'cmp w9, w8', 'ret')
        copy = self.function('validate_reference', 'mov w8, #0x700b0000', 'mov x0, x8', 'ret')
        load = self.function('validate_reference', 'movz x8, #0x700b, lsl #16', 'ldr w0, [x8]', 'ret')
        elsewhere = self.function('switch_diag_sd_evaluate', 'mov w8, #0x700b0000', 'cmp w9, w8', 'ret')

        def uses(text: str) -> bool:
            return any('700b' in p or 'controller' in p for p in inspect_disassembly(text, True))

        self.assertFalse(uses(compare))
        self.assertTrue(uses(copy))
        self.assertTrue(uses(load))
        self.assertTrue(uses(elsewhere))

    def test_d0_still_forbids_the_controller_address_everywhere(self) -> None:
        compare = self.function('validate_reference', 'movz x8, #0x700b, lsl #16', 'cmp w9, w8', 'ret')
        self.assertTrue(any('device address' in p for p in inspect_disassembly(compare, False)))

    def test_a_device_address_outside_the_validator_is_refused_in_d1_too(self) -> None:
        other = self.function('some_function', 'movz x8, #0x6000, lsl #16', 'ldr w0, [x8]', 'ret')
        self.assertTrue(any('device address' in p for p in inspect_disassembly(other, True)))


class Abi(unittest.TestCase):
    def test_the_loader_reads_the_block_the_script_writes(self) -> None:
        header = (ROOT / 'boot/diag/sd_loader/sd_report.h').read_text()

        def constant(name: str) -> int:
            return int(re.search(rf'#define {name}\s+(?:UINT64_C\()?(0x[0-9a-f]+|\d+)', header)[1], 0)

        self.assertEqual(constant('SWITCH_DIAG_SD_STATUS'), STATUS_BASE)
        self.assertEqual(constant('SWITCH_DIAG_SD_STATUS_MAGIC'), STATUS_MAGIC)
        self.assertEqual(constant('SWITCH_DIAG_SD_STATUS_VERSION'), STATUS_VERSION)
        self.assertEqual(constant('SWITCH_DIAG_SD_FLAGS_OFFSET'), FLAGS_OFFSET)
        self.assertEqual(constant('SWITCH_DIAG_SD_FLAG_COUNT'), len(STATUS_FLAGS))
        self.assertLess(FLAGS_OFFSET + 4 * len(STATUS_FLAGS), STATUS_BYTES)

    def test_the_status_block_is_inside_the_exporter_run_area_and_clear_of_its_image(self) -> None:
        self.assertGreaterEqual(STATUS_BASE, RUN + check_exporter(EXPORTER, SUMS).data_bytes)
        self.assertLessEqual(STATUS_BASE + STATUS_BYTES, RUN + EXPORTER_RUN[1])


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump'), 'no LLVM on PATH'
)
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [
        '--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-mgeneral-regs-only', '-ffreestanding',
        '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-O2', '-Wall', '-Wextra', '-Werror', '-DPROBE_QUIET=1',
        '-DSWITCH_DIAG_RESERVATIONS=1', '-DSWITCH_DIAG_SD=1',
    ]  # fmt: skip
    INCLUDES: ClassVar[list[str]] = [
        *('boot/diag/loader', 'boot/diag/sd_loader', 'boot/handoff', 'boot/loader', 'kits/SwitchBoard')
    ]
    SOURCES: ClassVar[list[str]] = [
        *(f'boot/diag/loader/{n}' for n in ('entry.S', 'diag.c', 'checks.c', 'report.c', 'reservations.c')),
        'boot/diag/sd_loader/sd_report.c',
        'boot/storage/sd_reference.c',
        'boot/diag/loader/exit.c',
        'boot/loader/memory.c',
    ]

    def test_it_links_and_passes_the_sd_inspection_and_d0_inspection_refuses_it(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            objects = []
            for i, source in enumerate(self.SOURCES):
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
            elf, linkmap = f'{scratch}/d1.elf', f'{scratch}/d1.map'
            linked = subprocess.run(
                ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
                capture_output=True, text=True, check=False,
            )  # fmt: skip
            self.assertEqual(0, linked.returncode, linked.stderr)
            dis = subprocess.run(
                ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
            ).stdout
            text = Path(linkmap).read_text()
            self.assertEqual([], inspect_map(text, True))
            self.assertEqual([], inspect_disassembly(dis, True))
            self.assertTrue(inspect_map(text, False))


if __name__ == '__main__':
    unittest.main()
