"""The pinned SD exporter's machine code: three barriers and no other system instruction, one record address.

Needs an AArch64 capable llvm-objdump on PATH and is skipped without one.
"""

import shutil
import unittest
from pathlib import Path

from exporter_audit import CONTROLLER, LINK, RECORD, audit, payload

ROOT = Path('.')
OBJDUMP = shutil.which('llvm-objdump')
IMAGE = payload((ROOT / 'bazel/pinned/firmware/exporter.uimg').read_bytes())
CRC32_CONSTANT = 0x76DC4190  # built in the CRC32 routine (0xa7001430..0xa7001440) with its neighbours; never an address
SOURCE = (ROOT / 'boot/storage/firmware_export/native.c').read_text()


class Header(unittest.TestCase):
    def test_it_loads_and_enters_where_the_script_and_the_package_expect(self) -> None:
        self.assertEqual((IMAGE.load, IMAGE.entry), (LINK, LINK))
        self.assertLessEqual(len(IMAGE.code), 0x10000)

    def test_the_source_names_the_same_record_address(self) -> None:
        self.assertIn('SWITCH_FW_EXPORT_PA', SOURCE)
        self.assertIn('0xa001f400', (ROOT / 'boot/storage/firmware_export/export.h').read_text())


@unittest.skipUnless(
    OBJDUMP
    and 'aarch64'
    in __import__('subprocess').run([OBJDUMP or 'true', '--version'], capture_output=True, text=True).stdout,
    'no AArch64 llvm-objdump',
)
class Code(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.facts = audit(IMAGE, OBJDUMP)

    def test_no_system_instruction_other_than_the_three_barriers(self) -> None:
        self.assertEqual(self.facts['system'], {})
        self.assertEqual(self.facts['barriers'], ['sy', 'sy', 'sy'])

    def test_the_record_address_is_built_and_no_other_device_or_firmware_address_is(self) -> None:
        built = self.facts['constants']
        self.assertIn(RECORD, built)
        self.assertIn(CONTROLLER, built, 'compared with, never read')
        peripherals = {c for c in built if 0x60000000 <= c < 0x80000000}
        self.assertEqual(peripherals, {CONTROLLER, CRC32_CONSTANT})

    def test_every_register_branch_is_a_call_through_the_reader_and_none_is_a_jump(self) -> None:
        self.assertEqual(self.facts['plain_branches_to_registers'], 0)
        self.assertEqual(self.facts['indirect_calls'], 14)


if __name__ == '__main__':
    unittest.main()
