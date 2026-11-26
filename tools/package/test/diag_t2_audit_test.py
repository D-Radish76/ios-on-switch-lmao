"""T2's static audit: the allowlist read from the sources, the linked accesses, and the cases that must be refused."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

import diag_images
import diag_t2_allowlist
from diag_mmio_audit import audit

ROOT = Path('.')
LOADER = ROOT / 'boot/diag/loader'
REGS = (LOADER / 't2_regs.c').read_text()
BUS = (LOADER / 't2_bus.c').read_text() + (ROOT / 'boot/diag/t2/t2.h').read_text()
RECORD = (ROOT / 'boot/diag/erista-t2-mmio-audit.txt').read_text()
HASHES = (ROOT / 'boot/diag/erista-diag-image-hashes.txt').read_text()


def allowlist(regs: str = REGS, bus: str = BUS) -> list[str]:
    return diag_t2_allowlist.audit(regs, bus)[1]


class Allowlist(unittest.TestCase):
    def test_the_sources_pass(self) -> None:
        self.assertEqual([], allowlist())

    def test_thirteen_addresses_and_the_four_car_words_are_read_only(self) -> None:
        entries = diag_t2_allowlist.table(REGS)
        self.assertEqual(13, len(entries))
        self.assertEqual(diag_t2_allowlist.ADDRESSES, {a for a, _ in entries.values()})
        car = {n: acc for n, (a, acc) in entries.items() if a < 0x70000000}
        self.assertEqual(4, len(car))
        self.assertTrue(all(acc == {'R'} for acc in car.values()))

    def test_a_car_write_is_refused(self) -> None:
        mutated = REGS.replace(
            '[T2_HW_CAR_CLK_OUT_ENB_L] = {CAR + 0x010, R}', '[T2_HW_CAR_CLK_OUT_ENB_L] = {CAR + 0x010, R | W}'
        )
        self.assertNotEqual(REGS, mutated)
        self.assertTrue([p for p in allowlist(mutated) if 'CAR' in p and 'read only' in p])

    def test_a_fourteenth_address_or_a_changed_one_is_refused(self) -> None:
        changed = REGS.replace('I2C1 + 0x98, R}', 'I2C1 + 0x9c, R}')
        self.assertNotEqual(REGS, changed)
        problems = allowlist(changed)
        self.assertTrue([p for p in problems if '0x7000c09c' in p])
        self.assertTrue([p for p in problems if '0x7000c098' in p])
        twice = REGS.replace('I2C1 + 0x98, R}', 'I2C1 + 0x94, R}')
        self.assertTrue([p for p in allowlist(twice) if 'twice' in p])

    def test_a_register_that_became_writable_is_refused(self) -> None:
        mutated = REGS.replace('[T2_HW_I2C1_STATUS] = {I2C1 + 0x1c, R}', '[T2_HW_I2C1_STATUS] = {I2C1 + 0x1c, R | W}')
        self.assertTrue([p for p in allowlist(mutated) if '0x7000c01c' in p and 'not in the reviewed set' in p])

    def test_an_extra_value_on_a_writable_register_is_refused(self) -> None:
        cases = {
            'case 0x00: case 0x15': ('case 0x00: case 0x15: case 0x13', '0x7000c00c'),
            'value == 0x2800u': ('value == 0x2801u || value == 0x2800u', '0x7000c000'),
            'value == 0x98u': ('value == 0x9au || value == 0x98u', '0x7000c004'),
        }
        for old, (new, address) in cases.items():
            mutated = REGS.replace(old, new, 1)
            self.assertNotEqual(REGS, mutated, old)
            self.assertTrue([p for p in allowlist(mutated) if address in p and 'beyond' in p], old)

    def test_a_missing_value_is_also_refused(self) -> None:
        mutated = REGS.replace('case 0xfe: ', '', 1)
        self.assertNotEqual(REGS, mutated)
        self.assertTrue([p for p in allowlist(mutated) if '0x7000c00c' in p])

    def test_the_one_shot_is_limited_to_pointer_0f_and_value_0(self) -> None:
        self.assertEqual([], allowlist())
        self.assertTrue(
            [
                p
                for p in allowlist(REGS, BUS.replace('reg != T2_ONE_SHOT || value != 0', 'reg != T2_ONE_SHOT'))
                if 'one-shot' in p
            ]
        )
        self.assertTrue(
            [p for p in allowlist(REGS, BUS.replace('T2_ONE_SHOT = 0x0f', 'T2_ONE_SHOT = 0x0e')) if '0x0f' in p]
        )
        two = BUS + '\n    x(CNFG_WRITE2);'
        self.assertTrue([p for p in allowlist(REGS, two) if 'exactly one place' in p])


class Record(unittest.TestCase):
    def test_the_saved_audit_passed_and_names_the_hash_of_the_recorded_image(self) -> None:
        self.assertIn('AUDIT PASSED (T2)', RECORD)
        self.assertIn('6. Flat image: register address words match the T2 list', RECORD)
        digest = re.search(r'flat image sha256 ([0-9a-f]{64})', RECORD).group(1)
        self.assertIn(f'diag_t2 {digest}', HASHES)
        self.assertIn('Not a hardware result', RECORD)

    def test_the_record_lists_one_tmp451_write(self) -> None:
        self.assertIn('one TMP451 write: pointer 0x0f, value 0x00', RECORD)
        self.assertIn('one-shot: pointer 0x0f (TMP451 one-shot start), value 0x00, one issuing call', RECORD)


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump') and shutil.which('llvm-objcopy'),
    'no LLVM on PATH',
)
class Linked(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.helper = cls

    FLAGS: ClassVar[list[str]] = [*diag_images.COPTS, '-DSWITCH_DIAG_T2=1']
    INCLUDES: ClassVar[list[str]] = [f'boot/{d}' for d in diag_images.INCLUDES] + ['kits/SwitchBoard']

    @classmethod
    def link(cls, scratch: str, extra: list[str]) -> tuple[str, str, bytes]:
        sources = [*(f'boot/{s}' for s in diag_images.COMMON + diag_images.T2 + diag_images.TAIL), *extra]
        objects = []
        for index, source in enumerate(sources):
            obj = f'{scratch}/{index}.o'
            command = ['clang', *cls.FLAGS, *(f for inc in cls.INCLUDES for f in ('-I', inc)), '-c', source, '-o', obj]
            subprocess.run(command, capture_output=True, text=True, check=True)
            objects.append(obj)
        elf, linkmap, flat = f'{scratch}/t2.elf', f'{scratch}/t2.map', f'{scratch}/t2.bin'
        subprocess.run(
            ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
            capture_output=True,
            text=True,
            check=True,
        )
        dis = subprocess.run(
            ['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True
        ).stdout
        subprocess.run(['llvm-objcopy', '-O', 'binary', elf, flat], check=True)
        return Path(linkmap).read_text(), dis, Path(flat).read_bytes()

    def test_the_audit_passes_on_the_real_link(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            _, dis, flat = self.helper.link(scratch, [])
        report, problems = audit(dis, flat, t2=True)
        self.assertEqual([], problems)
        self.assertIn('6. Flat image: register address words match the T2 list', report)
        self.assertTrue(any('T2: static table' in line for line in report))

    def test_a_second_caller_of_the_raw_store_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nvoid switch_t2_write32(uint32_t, uint32_t);\n'
                'void rogue(void) { switch_t2_write32(0x60006010u, 0); }\n'
            )
            _, dis, flat = self.helper.link(scratch, [str(rogue)])
        problems = audit(dis, flat, t2=True)[1]
        self.assertTrue([p for p in problems if 'switch_t2_write32 is called by rogue' in p])

    def test_a_second_caller_of_the_raw_load_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nuint32_t switch_t2_read32(uint32_t);\n'
                'uint32_t rogue(void) { return switch_t2_read32(0x70000804u); }\n'
            )
            _, dis, flat = self.helper.link(scratch, [str(rogue)])
        self.assertTrue([p for p in audit(dis, flat, t2=True)[1] if 'switch_t2_read32 is called by rogue' in p])

    def test_an_address_added_to_the_image_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            extra = Path(scratch) / 'extra.c'
            extra.write_text('const unsigned extra_address = 0x7000c0a0u;\n')
            _, dis, flat = self.helper.link(scratch, [str(extra)])
        self.assertTrue([p for p in audit(dis, flat, t2=True)[1] if '0x7000c0a0' in p])


if __name__ == '__main__':
    unittest.main()
