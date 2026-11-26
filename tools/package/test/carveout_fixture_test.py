"""The Erista carveout table, the host test's fixture and the measured log say the same 25 things in the same order."""

import re
import unittest
from pathlib import Path

from memory_layout import mc_registers

ROOT = Path('.')
CARVEOUT = (ROOT / 'boot/loader/carveout.h').read_text()
FIXTURE = (ROOT / 'boot/loader/test/erista_mc_fixture.h').read_text()
LOG = (ROOT / 'boot/diag/erista-first-run.log').read_text()
IDENTITY = {0x70000804, 0x7000F9D8}


def table(source: str, name: str) -> list[tuple[int, int]]:
    body = source.split(name, 1)[1].split('};', 1)[0]
    return [(int(a, 16), int(v, 0)) for a, v in re.findall(r'\{(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+|\d+)\}', body)]


class Erista(unittest.TestCase):
    def test_the_log_rows_in_order_are_the_fixture(self) -> None:
        logged = [(a, v) for a, v in mc_registers(LOG).items() if a not in IDENTITY]
        self.assertEqual(len(logged), 25)
        self.assertEqual(table(FIXTURE, 'kEristaMeasured'), logged)

    def test_the_gate_table_is_the_fixture(self) -> None:
        self.assertEqual(table(CARVEOUT, 'switch_carveout_erista'), table(FIXTURE, 'kEristaMeasured'))

    def test_the_register_list_is_the_oleds_list(self) -> None:
        oled = table(CARVEOUT, 'switch_carveout_oled')
        erista = table(CARVEOUT, 'switch_carveout_erista')
        self.assertEqual([a for a, _ in oled], [a for a, _ in erista])

    def test_the_count_constant_is_the_tables_length(self) -> None:
        count = int(re.search(r'SWITCH_CARVEOUT_REGISTERS (\d+)', CARVEOUT).group(1))
        self.assertEqual(count, len(table(CARVEOUT, 'switch_carveout_oled')))
        self.assertEqual(count, len(table(CARVEOUT, 'switch_carveout_erista')))


if __name__ == '__main__':
    unittest.main()
