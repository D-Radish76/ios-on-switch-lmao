"""Check calls compiled against the real framebuffer header and kernel SDK."""

import argparse
import struct
import unittest
from pathlib import Path

from kits.SwitchABI import gen
from tools.common import macho

ROOT = Path(__file__).resolve().parents[1]
VTABLE = '__ZTV25IOMobileFramebufferLegacy'


class FramebufferCompletionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.data = Path(ARGS.object).read_bytes()
        cls.image = macho.macho(cls.data)
        cls.symbols = macho.symbols(cls.data, cls.image)
        cls.interface = gen.load(ROOT / 'interface/IOMobileFramebufferLegacy.json')
        cls.table = gen.load(ARGS.table)

    def function(self, name: str) -> tuple[tuple[int, ...], dict[int, str]]:
        symbol = next(s for s in self.symbols if s.name == '_' + name)
        section = [s for seg in self.image['segments'] for s in seg['sections']][symbol.sect - 1]
        end = min(
            [s.value for s in self.symbols if s.sect == symbol.sect and s.value > symbol.value]
            + [section['va'] + section['size']]
        )
        offset = macho.backed_offset(self.image['segments'], symbol.value, end - symbol.value, len(self.data))
        references = {}
        for index in range(section['nreloc']):
            at, info = struct.unpack_from('<iI', self.data, section['reloff'] + 8 * index)
            if info >> 27 & 1 and symbol.value <= section['va'] + at < end:
                references[section['va'] + at - symbol.value] = self.symbols[info & 0xFFFFFF].name
        words = struct.unpack_from('<' + 'I' * ((end - symbol.value) // 4), self.data, offset)
        return words, references

    def assert_dispatch(self, name: str, slot: int, original: bool = False) -> None:
        registers = {0: 'display'}
        called = []
        words, references = self.function(name)
        for index, word in enumerate(words):
            reference = references.get(4 * index)
            if word & 0x9F000000 == 0x90000000 and reference == VTABLE:
                registers[word & 31] = 'original-got'
            elif word & 0xFFC00000 == 0xF9400000:
                source = registers.get((word >> 5) & 31)
                offset = ((word >> 10) & 0xFFF) * 8
                value = 'vtable' if source == 'display' and offset == 0 else None
                if source == 'original-got' and reference == VTABLE:
                    value = 'original-vtable'
                elif source == 'vtable' and not original:
                    value = offset // 8
                elif source == 'original-vtable' and original:
                    value = offset // 8 - 2
                registers[word & 31] = value
            elif word & 0xFFFFFC1F in (0xD61F0000, 0xD63F0000):
                called.append(registers.get((word >> 5) & 31))
        self.assertEqual(called, [slot], name)

    def test_queued_dispatch_uses_the_pinned_slot(self):
        slot = {'21H461': 312, '24B119': 330}[self.table['kind']]
        self.assertEqual(self.table['layout'][str(slot)], 'completeQueuedSwap')
        self.assert_dispatch('switch_framebuffer_dispatch_queued', slot)

    def test_current_dispatch_remains_separate(self):
        slot = next(int(s) for s, name in self.table['layout'].items() if name == 'completeCurrentSwap')
        self.assert_dispatch('switch_framebuffer_dispatch_current', slot)
        self.assertNotEqual(slot, {'21H461': 312, '24B119': 330}[self.table['kind']])

    def test_qualified_call_dispatches_through_the_original_vtable(self):
        slot = {'21H461': 312, '24B119': 330}[self.table['kind']]
        self.assert_dispatch('switch_framebuffer_import_queued', slot, original=True)

    def test_a_neighboring_slot_is_rejected(self):
        slot = {'21H461': 312, '24B119': 330}[self.table['kind']]
        for name, original in (
            ('switch_framebuffer_dispatch_queued', False),
            ('switch_framebuffer_import_queued', True),
        ):
            with self.subTest(name=name), self.assertRaises(AssertionError):
                self.assert_dispatch(name, slot + 1, original=original)

    def test_qualified_import_binds_the_compilers_vtable_symbol(self):
        imports = {s.name for s in self.symbols if macho.undefined_external(s.type)}
        self.assertEqual(imports, {VTABLE})
        bindings = gen.bindings([(self.interface, self.table, ARGS.table)], [ARGS.object])
        address = gen.number(self.table['addresses']['vtable'])
        slid = (address + gen.number(self.table['slide'])) & gen.M64
        self.assertIn(f'.set {VTABLE}, {slid:#x}', bindings)
        self.assertNotIn('completeCurrentSwap', bindings)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--object', required=True)
    parser.add_argument('--table', required=True)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
