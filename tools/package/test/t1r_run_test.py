"""The first T1-R run on the Erista (boot/diag/erista-t1r-run.log): what it recorded, and what decoding it gives."""

import re
import unittest
from pathlib import Path

ROOT = Path('.')
TEXT = (ROOT / 'boot/diag/erista-t1r-run.log').read_text()
BODY = '\n'.join(line for line in TEXT.splitlines() if not line.startswith('#'))
POINTS = ('P1', 'P2')
ADDRESSES = ('60006124', '60006128', '60006004', '60006010', '600060A0')
HEKATE = {1: 0xC0000003, 2: 0xC0000000, 3: 0xC0000000, 4: 0xC0000003, 5: 0xC0000000, 6: 0xC0000003}
T1_V2_RST_L = 0x049102CA
T1_V2_ENB_L = 0x9802D131
T1_V2_I2C1 = {'CNFG': 0x2842, 'STATUS': 0, 'INT': 0, 'DIV': 0x50001, 'LOAD': 4}
CLK_L_I2C1 = 1 << 12


def section(name: str) -> list[str]:
    m = re.search(rf'^{name}\b.*?\n(.*?)(?=^P\d |\Z)', BODY, re.MULTILINE | re.DOTALL)
    assert m, name
    return m.group(1).splitlines()


def md(point: str) -> dict[str, int]:
    return {a: int(v, 16) for a, v in (line.split() for line in section(point))}


def source(bus: int) -> tuple[int, str]:
    m = re.search(
        rf'CLK_SOURCE_I2C{bus} ([0-9A-F]{{8}}) FIELD 31:30 (\d) DIVIDER ([0-9A-F]{{4}})\. HEKATE WRITES (\w+)\. (\w+)',
        BODY,
    )
    assert m, bus
    return int(m.group(1), 16), m.group(5)


class Recorded(unittest.TestCase):
    def test_the_board_is_an_erista_with_boot_zero_and_the_run_completed(self) -> None:
        self.assertIn('BOARD ERISTA T210B01=0 SKU=0 BOOT=0', BODY)
        self.assertIn('EL1 STATE PASS', BODY)
        self.assertIn('T1R COMPLETE CODE 0000. READ ONLY. NO I2C TRANSFER. NO REGISTER WRITE', BODY)
        self.assertIn('BOOT STAYS 0.', BODY)

    def test_p1_and_p2_read_the_five_addresses_in_the_scripts_order(self) -> None:
        for point in POINTS:
            self.assertEqual(list(ADDRESSES), list(md(point)))

    def test_the_three_points_agree_on_every_register_they_share(self) -> None:
        p1, p2 = md('P1'), md('P2')
        self.assertEqual(p1, p2)
        self.assertEqual(p1['60006124'], source(1)[0])
        self.assertEqual(p1['60006128'], source(5)[0])
        self.assertEqual(p1['60006004'], int(re.search(r'RST_L ([0-9A-F]{8})', BODY).group(1), 16))
        self.assertEqual(p1['60006010'], int(re.search(r'ENB_L ([0-9A-F]{8})', BODY).group(1), 16))
        self.assertEqual(p1['600060A0'], int(re.search(r'PLLP BASE ([0-9A-F]{8})', BODY).group(1), 16))

    def test_no_temperature_or_card_identity_is_in_the_values(self) -> None:
        self.assertNotRegex(BODY, r'DEG|CELSIUS|\d\s*°| C\b')
        self.assertNotIn('CID', BODY)


class Decoded(unittest.TestCase):
    def test_every_source_field_is_zero_and_no_bus_holds_hekates_word(self) -> None:
        for bus, written in HEKATE.items():
            value, verdict = source(bus)
            self.assertEqual(0, value >> 30, bus)
            self.assertEqual('DIFFERENT', verdict)
            self.assertNotEqual(value, written)

    def test_only_i2c1_and_i2c5_hold_a_divider(self) -> None:
        self.assertEqual({1: 3, 2: 0, 3: 0, 4: 0, 5: 3, 6: 0}, {bus: source(bus)[0] & 0xFFFF for bus in HEKATE})

    def test_i2c1_carries_hekates_divider_and_i2c5_does_not(self) -> None:
        self.assertEqual(HEKATE[1] & 0xFFFF, source(1)[0] & 0xFFFF)
        self.assertNotEqual(HEKATE[5] & 0xFFFF, source(5)[0] & 0xFFFF)

    def test_enable_and_reset_words_match_t1_v2_except_for_one_bit(self) -> None:
        rst = int(re.search(r'RST_L ([0-9A-F]{8})', BODY).group(1), 16)
        enb = int(re.search(r'ENB_L ([0-9A-F]{8})', BODY).group(1), 16)
        self.assertEqual(T1_V2_ENB_L, enb)
        self.assertEqual(1 << 22, rst ^ T1_V2_RST_L)
        self.assertTrue(enb & CLK_L_I2C1)
        self.assertFalse(rst & CLK_L_I2C1)

    def test_the_i2c1_controller_words_equal_t1_v2s(self) -> None:
        line = re.search(r'I2C1 CNFG (\w+) STATUS (\w+) INT (\w+) DIV (\w+) LOAD (\w+)', BODY)
        self.assertEqual(list(T1_V2_I2C1.values()), [int(g, 16) for g in line.groups()])

    def test_pllp_is_enabled_and_locked_by_hekates_bit_names_and_no_frequency_is_asserted(self) -> None:
        base = int(re.search(r'PLLP BASE ([0-9A-F]{8})', BODY).group(1), 16)
        self.assertTrue(base & (1 << 30))
        self.assertTrue(base & (1 << 27))
        self.assertFalse(base & (1 << 31))
        self.assertIn('NO FREQUENCY CLAIMED', BODY)
        self.assertNotRegex(BODY, r'MHZ|KHZ')


if __name__ == '__main__':
    unittest.main()
