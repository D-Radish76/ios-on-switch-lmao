"""The Erista carveout values, the host test's fixture and the measured log say the same 25 things in the same order."""

import re
import unittest
from pathlib import Path

from memory_layout import mc_registers

ROOT = Path('.')
CARVEOUT = (ROOT / 'boot/loader/carveout.h').read_text()
FIXTURE = (ROOT / 'boot/loader/test/erista_mc_fixture.h').read_text()
LOG = (ROOT / 'boot/diag/erista-first-run.log').read_text()
IDENTITY = {0x70000804, 0x7000F9D8}
BASE = int(re.search(r'SWITCH_CARVEOUT_MC_BASE (0x[0-9a-fA-F]+)u', CARVEOUT).group(1), 16)


def numbers(source: str, name: str) -> list[int]:
    body = source.split(name, 1)[1].split('};', 1)[0]
    return [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|(?<![\w])\d+(?![\w])', body.split('{', 1)[1])]


def fixture() -> list[tuple[int, int]]:
    body = FIXTURE.split('kEristaMeasured', 1)[1]
    return [(int(a, 16), int(v, 16)) for a, v in re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+)\}', body)]


def gate(name: str) -> list[tuple[int, int]]:
    offsets = numbers(CARVEOUT, 'switch_carveout_offset[')
    return [(BASE + o, v) for o, v in zip(offsets, numbers(CARVEOUT, name + '['), strict=True)]


class Erista(unittest.TestCase):
    def test_the_log_rows_in_order_are_the_fixture(self) -> None:
        logged = [(a, v) for a, v in mc_registers(LOG).items() if a not in IDENTITY]
        self.assertEqual(len(logged), 25)
        self.assertEqual(fixture(), logged)

    def test_the_gate_values_are_the_fixture(self) -> None:
        self.assertEqual(gate('switch_carveout_ERISTA'), fixture())

    def test_the_register_list_is_shared_and_each_board_has_its_own_values(self) -> None:
        self.assertEqual(len(gate('switch_carveout_OLED')), 25)
        self.assertEqual([a for a, _ in gate('switch_carveout_OLED')], [a for a, _ in gate('switch_carveout_ERISTA')])

    def test_the_count_constant_is_the_tables_length(self) -> None:
        count = int(re.search(r'SWITCH_CARVEOUT_REGISTERS (\d+)', CARVEOUT).group(1))
        self.assertEqual(count, 25)
        self.assertEqual(len(numbers(CARVEOUT, 'switch_carveout_offset[')), count)


if __name__ == '__main__':
    unittest.main()
