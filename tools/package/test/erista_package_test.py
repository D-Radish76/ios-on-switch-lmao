"""//boot:erista_package: its own tree, DTB, directory and one core, and nothing a console could boot from.

boot/BUILD.bazel is evaluated with recording stand-ins for its rules, so what is checked is the attribute each target
gets. The refusals themselves are boot_script_board_test (U-Boot) and carveout_gate_test (the loader).
"""

import re
import unittest
from pathlib import Path

from boot_script import board_gate, boards

BUILD = Path('boot/BUILD.bazel')
HEADER = Path('kits/SwitchBoard/SwitchBoard.h').read_text()
LOADER = Path('boot/loader/loader.c').read_text()
OWN = {'name', 'adt', 'dtb', 'smp', 'out', 'loader_defines', 'tags', 'target_compatible_with', 'visibility'}


class Select:
    def __init__(self, branches):
        self.branches = branches

    def __or__(self, other):
        return Select(('or', self.branches, getattr(other, 'branches', other)))

    def __ror__(self, other):
        return Select(('or', getattr(other, 'branches', other), self.branches))

    def __eq__(self, other):
        return isinstance(other, Select) and self.branches == other.branches


def targets():
    """name -> (rule, attributes) of every target boot/BUILD.bazel declares."""
    found = {}

    def rule(kind):
        def declare(*_, **attributes):
            if 'name' in attributes:
                found[attributes['name']] = (kind, attributes)

        return declare

    namespace = {
        'select': lambda branches, *_, **__: Select(branches),
        'glob': lambda *_, **__: [],
        'selects': type('selects', (), {'config_setting_group': staticmethod(rule('config_setting_group'))}),
        'BOOTSTRAP_PACKAGES': {'package': 'source'},
        'OVERLAY_BINARIES': {'target': 'source'},
    }
    for kind in ('package', 'exports_files', 'filegroup', 'alias', 'label_flag'):
        namespace[kind] = rule(kind)

    def load(_, *names, **aliases):
        for name in (*names, *aliases):
            namespace.setdefault(name, rule(name))

    namespace['load'] = load
    exec(compile(BUILD.read_text(), str(BUILD), 'exec'), namespace)
    return found


TARGETS = targets()
OLED = TARGETS['package'][1]
ERISTA = TARGETS['erista_package'][1]
ROWS = TARGETS['erista_rows_work'][1]


class EristaPackageTest(unittest.TestCase):
    def test_the_erista_has_its_own_tree_dtb_directory_and_one_core(self):
        self.assertEqual(TARGETS['erista_package'][0], 'boot_package')
        self.assertEqual(ERISTA['adt'], ':erista_adt')
        self.assertTrue(ERISTA['dtb'].endswith('erista-sku0.dtb'))
        self.assertEqual(ERISTA['smp'], ':single_core')
        self.assertEqual(TARGETS['single_core'], ('bool_setting', {'name': 'single_core', 'build_setting_default': False}))
        self.assertNotIn(ERISTA['out'], (OLED.get('out', 'ios'), ROWS['out']))

    def test_the_oled_package_is_what_it_was(self):
        self.assertEqual(OLED['adt'], ':adt')
        self.assertTrue(OLED['dtb'].endswith('oled-sku3.dtb'))
        self.assertNotIn('loader_defines', OLED)
        self.assertNotIn('out', OLED)
        self.assertNotIn('tags', OLED)
        self.assertNotIn('erista', repr({k: v for k, v in OLED.items() if k != 'dtb'}).lower())

    def test_everything_else_is_shared(self):
        for other in (ERISTA, ROWS):
            self.assertEqual({k: v for k, v in other.items() if k not in OWN}, {k: v for k, v in OLED.items() if k not in OWN})

    def test_it_is_opt_in_and_only_from_a_local_build(self):
        for attributes in (ERISTA, ROWS):
            self.assertEqual(attributes['tags'], ['manual'])
            self.assertEqual(
                attributes['target_compatible_with'],
                Select({'//install:from_local': [], '//conditions:default': ['@platforms//:incompatible']}),
            )

    def test_the_size_check_links_a_loader_and_offers_nothing_else(self):
        self.assertEqual(ROWS['loader_defines'], ['-DSWITCH_CARVEOUT_ALL_ROWS=1'])
        self.assertNotIn('loader_defines', ERISTA)
        self.assertEqual(ROWS['visibility'], ['//visibility:private'])
        self.assertEqual(TARGETS['erista_loader_all_rows'][0], 'filegroup')
        self.assertEqual(TARGETS['erista_loader_all_rows'][1]['srcs'], [':erista_rows_work'])
        self.assertEqual(TARGETS['erista_loader_all_rows'][1]['output_group'], 'loader')

    def test_the_erista_row_does_not_boot_so_both_stages_refuse_it(self):
        self.assertEqual([(name, boot) for name, _, _, boot in boards(HEADER)], [('oled', True), ('erista', False)])
        self.assertNotIn('erista', board_gate(HEADER))
        refusal = re.search(r'if \(!board->boot\)\s*refuse\(0x306\);', LOADER)
        self.assertIsNotNone(refusal)
        self.assertLess(refusal.start(), LOADER.index('native_gate(board)'))


if __name__ == '__main__':
    unittest.main()
