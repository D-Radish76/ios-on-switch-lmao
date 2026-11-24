"""The boot script's board check comes from kits/SwitchBoard/SwitchBoard.h and nothing else.

The OLED keeps exactly the check it had (t210b01 1, sku 3); a row with boot 0 (the Erista) adds nothing, so its console
still ends in WRONG_BOARD; a row someone later enables adds one clause; a table the tool cannot read stops the build.
"""

import re
import unittest
from pathlib import Path

from boot_script import board_gate, boards, boot_script
from native_oled_adt import COMPATIBLE, IDENTITIES, MODEL

HEADER = Path('kits/SwitchBoard/SwitchBoard.h').read_text()
TEMPLATE = Path('boot/u-boot-script.txt').read_text()
CONSTANTS = {'PACKED_BYTES': 0x1000, 'STREAM_BYTES': 0x800, 'STAGE_ID': 0x1122334455667788}

OLED_GATE = """setenv handoff_status WRONG_BOARD
# oled
if test "${t210b01}" = "1" -a "${sku}" = "3"; then
    setenv handoff_status READY
fi"""


def table(*rows):
    body = ' \\\n'.join(f'    X({name.upper()}, "{name}", {t}, {sku}, {boot}, 0, NULL, NULL)' for name, t, sku, boot in rows)
    return f'#define SWITCH_BOARDS(X) \\\n{body}\n'


def identity(name):
    row = re.search(rf'X\(\w+, "{name}", [^\n]*?, ("[^"]*"|NULL), ("[^"]*"|NULL)\)', HEADER)
    return tuple(None if v == 'NULL' else v.strip('"') for v in row.groups())


class BoardGateTest(unittest.TestCase):
    def test_the_shipped_table_boots_the_oled_only(self):
        self.assertEqual(boards(HEADER), [('oled', 1, 3, True), ('erista', 0, 0, False)])
        self.assertEqual(board_gate(HEADER), OLED_GATE)

    def test_the_script_keeps_the_oled_check_and_refuses_everything_else(self):
        script = boot_script(TEMPLATE, CONSTANTS, 0x1000, HEADER)
        self.assertIn(OLED_GATE, script)
        self.assertNotIn('erista', script)
        self.assertNotRegex(script, r'@[A-Z_0-9]+@')
        self.assertEqual(script.count('WRONG_BOARD'), 1)
        self.assertEqual(script.count('t210b01'), 2)  # the comment and the check
        before = script.index('WRONG_BOARD')
        self.assertLess(before, script.index('FIRMWARE_MISMATCH'), 'the board check runs before the firmware check')
        self.assertIn('if test "${handoff_status}" = "READY"; then', script[before:])

    def test_an_enabled_row_adds_one_clause_and_leaves_the_oled_alone(self):
        gate = board_gate(table(('oled', 1, 3, 1), ('erista', 0, 0, 1)))
        self.assertTrue(gate.startswith(OLED_GATE))
        self.assertEqual(gate.count('setenv handoff_status READY'), 2)
        self.assertIn('if test "${t210b01}" = "0" -a "${sku}" = "0"; then', gate)

    def test_a_table_that_boots_nothing_still_refuses(self):
        self.assertEqual(board_gate(table(('erista', 0, 0, 0))), 'setenv handoff_status WRONG_BOARD')

    def test_a_table_the_tool_cannot_read_stops_the_build(self):
        for bad in ('', '#define SWITCH_BOARDS(X)\n', '#define SWITCH_BOARDS(X) \\\n    X(OLED, "oled", one, 3, 1, 0)\n'):
            with self.assertRaises(SystemExit):
                boards(bad)

    def test_a_template_without_the_gate_stops_the_build(self):
        with self.assertRaises(SystemExit):
            boot_script(TEMPLATE.replace('@BOARD_GATE@', ''), CONSTANTS, 0x1000, HEADER)

    def test_every_clause_names_a_board_of_the_table(self):
        names = {name for name, *_ in boards(HEADER)}
        self.assertEqual(set(re.findall(r'^# (\w+)$', board_gate(HEADER), re.M)), {n for n, *_, boot in boards(HEADER) if boot} & names)


class BoardIdentityTest(unittest.TestCase):
    def test_the_oled_row_is_what_the_native_tree_says(self):
        model, compatible = identity('oled')
        self.assertEqual(model, MODEL)
        self.assertIn(compatible, COMPATIBLE)

    def test_the_erista_row_is_what_its_reported_tree_says(self):
        model, compatible = identity('erista')
        self.assertEqual(model, IDENTITIES['erista'].model)
        self.assertIn(compatible, IDENTITIES['erista'].compatible)

    def test_every_row_has_an_identity_and_nothing_else_does(self):
        self.assertEqual({name for name, *_ in boards(HEADER)}, set(IDENTITIES))


if __name__ == '__main__':
    unittest.main()
