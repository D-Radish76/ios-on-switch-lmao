# hekate 6.5.5: what the release does to the I2C clocks after hw_init

Static reading of the unpacked release image (`ERISTA_HEKATE_BINARY.md`, `tools/package/hekate_unpack.py`). Nothing was run
and nothing was measured after a write. `boot` stays 0 and no hardware, clock or firmware change comes from this.
`tools/package/hekate_car_scan.py` produced the lists; its output for this image is `boot/diag/erista-hekate-car-scan.txt`.
Addresses are image addresses (base `0x40008000`). Function boundaries come from `push {.., lr}` and are approximate:
leaf functions without it are folded into the function before them, and literal pools that decode as `bl` add false
edges. A call graph built on that over-reports reachability, so it is not used below.

## What is established

| Question | Answer from the image |
| --- | --- |
| Where are the I2C clock sources written? | Only through `clock_enable` (`0x4000bcf8`), which takes a row of the 8 byte table at `0x40022ff0`. The only user of that table is `clock_enable_i2c` (`0x4000bf44`). |
| Who calls `clock_enable_i2c`? | Two call sites, both in `hw_init` (`0x400160d8`): `0x40016134` with I2C1 (index 0) and `0x4001613a` with I2C5 (index 4). |
| Who calls `i2c_init` (`0x4000d1f0`)? | Two call sites in `hw_init`: `0x40016144` (index 4, I2C5) and `0x40016152` (index 0, I2C1). |
| Is there a `clock_disable_i2c` or a second use of the I2C rows? | No second reference to the table, so the build has none. |
| Do other `clock_enable` users touch I2C? | The other 12 call sites use tables at `0x400214c0` to `0x40023040`, other devices (one is the security engine's `CLK_SOURCE_SE`). The table address is a constant in each. |
| Direct stores to `CLK_SOURCE_I2C1`/`5` or the other four? | None. No literal for any of the six addresses exists; they are reached only as `base + row.source`. |
| `PLLP_OUTB` | One store, in `hw_init` (`0x400160d8`), value `0x00030003`, before the I2C code. |
| `hw_deinit` (`0x400161bc`) | Calls the BPMP clock helper (`0x4000b904`, writes `SCLK_BURST_POLICY` and `CLK_SYSTEM_RATE` through `0x4000b8ac`), `minerva_deinit` (`0x400107ec`), the MMU disable (`0x4000b88c`), `hw_config_arbiter` (`0x40016078`), the chip id read (`0x40016060`), and on the display branch `clock_disable_host1x` (`0x4000bfbc`) and `display_end` (`0x4000d014`). Then it sets `CLK_ENB_V_SET` and `CLK_ENB_Y_SET` (AHUB, APE). No I2C call and no I2C bit. |
| `_l4t_late_hw_config` (`0x400172c4`) | Touches the system counters (`0x700f0fd0` and neighbours), PMC (`0x7000ec44`, `0x7000e4f8`, `0x7000e6c0`), `MSELECT` (`0x6000c110`) and the MC (`0x70019000`). It has no call and no CAR address. |
| End of `launch_l4t` | After `hw_deinit` (call at `0x40017c72`) and the late config, it calls a helper that does `bx r3` into `BPMPFW_ENTRYPOINT`, then `bpmp_halt`. |
| Direct CAR stores with a known value that hit an I2C1 or I2C5 bit (`RST_DEV_L` and `CLK_ENB_L` bit 12, `RST_DEV_H` and `CLK_ENB_H` bit 15) | None among the 128 stores in the image. The scan flags such a store. Stores through a computed address or a loop are not seen; 43 stores with unknown values are to PLL, EMC, `CLK_SOURCE_*` of non I2C devices and `LVL2` registers, none to the reset or enable words. |

The bit values found in the reset and enable words match hekate's source: kfuse (`H` bit 8), display end (`H` bits 16 and
24, `L` bit 27), memory controller enable (`H` bits 0 and 25), the `hw_init` clock block (`L` bits 27 and 28, `CLK_OUT_ENB_L`
`0x80000130`, before the I2C code).

## Not established

- Reachability in the strict sense. The scan lists stores in functions, not paths.
- Anything in `bpmpfw.bin`, BL31 and U-Boot: see `ERISTA_BPMPFW.md` and `ERISTA_BOOTCHAIN_CAR.md`.
- Indirect stores: function pointers (menus, the payload launcher), a `str` through a register loaded from memory, and
  stores in code that runs from a copy (the relocated payload code at `0xC0000000`).
- Whether the release hekate behaves like this source in every path. The Nyx start differs (`Nyx` runs ARM mode code with an
  extra deinit; `ERISTA_T1H_DESIGN.md`), and hekate's own `hw_init` runs twice there.

## What this does to the open question

The release `hw_init` writes `C0000003` to `CLK_SOURCE_I2C1` and `C0000000` to `CLK_SOURCE_I2C5`, enables both, and sets
the I2C1 divisor to `00050001`. T1-R read `00000003` for both and `00050001` for the divisor. This image has no later store
that changes either source. That does not show that something else wrote them. Candidates stay open:

1. A software writer after hekate: `bpmpfw.bin`, BL31 or U-Boot. Read offline with no resolved write
   (`ERISTA_BPMPFW.md`, `ERISTA_BOOTCHAIN_CAR.md`), with the limits stated there (indirect accesses are not covered).
2. A hardware or reset side effect. The I2C1 divisor kept hekate's value, which a reset of the I2C1 controller would have
   cleared, so a device reset of I2C1 after `i2c_init` is not suggested by the data. A reset of only the CAR source word is
   not excluded: the CAR does not belong to the controller.
3. A power state change (a warm path through the PMC or the BPMP) that restores register defaults. The measured I2C2, 3, 4
   and 6 words, which hekate never writes, read `00000000`. If `00000000` is the default, a restore would give `00000000`,
   not `00000003`, which weakens this candidate for the two buses. It does not remove it: the two buses' defaults may differ
   and the reset values are not documented in anything read.
4. The first-run reading was wrong (a decode error). The words were read in three places with the same result, which makes
   it unlikely.

I2C5 changing its divider from 0 to 3 is what needs explaining by whichever candidate is chosen. No reset-to-zero explains it.

## RST_DEVICES_L bit 22

T1 v2 and T1-R differ in `RST_DEVICES_L` bit 22 (USBD in hekate's names). The image has a function (`0x40018856`, no direct
`bl` caller, so reached through a pointer if at all) that sets `RST_DEV_L_SET` bit 22 and `RST_DEV_H_SET` bits 1, 2 and 26,
which reads as USB teardown. Whether it ran in either run is not known. It is one possible source of the difference.

## Reproduce

```
python3 tools/package/hekate_unpack.py hekate_ctcaer_6.5.5.bin -o unpacked.bin
llvm-objcopy -I binary -O elf32-littlearm --rename-section .data=.text,alloc,load,code unpacked.bin u.o
ld.lld -Ttext=0x40008000 -e 0 u.o -o u.elf
llvm-objdump -d --triple=thumbv4t --no-show-raw-insn u.elf | sed 's/<_binary[^>]*>//' > u.dis
python3 tools/package/hekate_car_scan.py unpacked.bin u.dis --target 0x4000bcf8 --target 0x4000bf44 --target 0x4000d1f0 --target 0x400161bc
```
