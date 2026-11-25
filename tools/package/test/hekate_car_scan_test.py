"""hekate_car_scan.py on a synthetic disassembly."""

import struct
import unittest

from hekate_car_scan import callers_of, car_stores, function_starts, parse, touches_i2c

BASE = 0x40008000
DIS = """
40008000:      	push	{r4, lr}
40008002:      	ldr	r3, [pc, #0x14]         @ 0x40008018
40008004:      	movs	r4, #0x80
40008006:      	lsls	r4, r4, #0x5
40008008:      	str	r4, [r3]
4000800a:      	bl	0x40008020
4000800e:      	pop	{r4}
40008020:      	push	{lr}
40008022:      	ldr	r3, [pc, #0xc]          @ 0x40008018
40008024:      	movs	r2, #0x1
40008026:      	str	r2, [r3, #0x4]
"""


def image() -> bytes:
    data = bytearray(0x40)
    struct.pack_into('<I', data, 0x18, 0x60006000 + 0x320)
    return bytes(data)


class Scan(unittest.TestCase):
    def test_a_store_to_a_known_car_address_with_a_known_value_is_reported(self) -> None:
        insns, _ = parse(DIS)
        stores = car_stores(image(), BASE, insns)
        self.assertEqual((0x40008000, 0x40008008, 0x320, 0x1000), stores[0])

    def test_the_pool_is_reloaded_per_function_and_offsets_add(self) -> None:
        insns, _ = parse(DIS)
        stores = car_stores(image(), BASE, insns)
        self.assertEqual((0x40008020, 0x40008026, 0x324, 1), stores[1])

    def test_callers_are_mapped_to_their_function(self) -> None:
        insns, calls = parse(DIS)
        self.assertEqual([0x40008000], callers_of(calls, function_starts(insns), 0x40008020))

    def test_an_i2c_bit_in_an_enable_register_is_flagged(self) -> None:
        self.assertEqual('TOUCHES I2C BIT', touches_i2c(0x320, 1 << 12))
        self.assertEqual('', touches_i2c(0x320, 1 << 11))
        self.assertEqual('TOUCHES I2C BIT', touches_i2c(0x328, 1 << 15))
        self.assertEqual('value unknown', touches_i2c(0x300, None))
        self.assertEqual('', touches_i2c(0x124, 0xC0000003))


if __name__ == '__main__':
    unittest.main()
