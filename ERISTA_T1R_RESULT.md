# T1-R first run (2026-11-26): result and investigation

Erista, hekate 6.5.5, started from Nyx "More configs", commit `51e48829f`. The run completed, the loader reset, hekate
returned by itself, and Nyx then showed 38.7 C (operator report). Hekate was back about 24 s after the video started (25.73 s
video). Values are in `boot/diag/erista-t1r-run.log` and decoded by `tools/package/test/t1r_run_test.py`; the loader's screen
text for them is reproduced by `T1RReport.TheRecordedErista20261126RunShowsAllSixBusesDifferent`.

## Measured

| Register | P1 | P2 | P3 |
| --- | --- | --- | --- |
| `CLK_SOURCE_I2C1` 0x60006124 | 00000003 | 00000003 | 00000003 |
| `CLK_SOURCE_I2C5` 0x60006128 | 00000003 | 00000003 | 00000003 |
| `RST_DEVICES_L` | 04D102CA | 04D102CA | 04D102CA |
| `CLK_OUT_ENB_L` | 9802D131 | 9802D131 | 9802D131 |
| `PLLP_BASE` | 48115408 (last digit not legible) | same | 48115408 |

P3 only: `CLK_SOURCE_I2C2/3/4/6` are all 00000000, `PLLP_OUTA` 00000003, `PLLP_OUTB` 00030003, and the I2C1 words
`CNFG` 00002842, `STATUS` 0, `INT` 0, `DIV` 00050001, `LOAD` 00000004 (the same as T1 v2).

What it shows: the source field (bits 31:30) is 0 on all six buses, and nothing changed between U-Boot before `bootm start`,
after `bootm prep`, and the loader at EL1. Only I2C1 and I2C5 carry a divider (3), and they hold the same word. The two buses
hekate sets up both lost its value, so this is not specific to I2C1, and it is not from `bootm` or the hand-off. The screen
marks I2C2, 3, 4 and 6 DIFFERENT because it compares against hekate's table, but hekate's source in this tree never touches
those four buses, so their 0 is their untouched state if the reset value is 0 (not verified).

What it does not show: who wrote the values, the PLLP frequency, what source 0 means on this silicon, or any bus speed.

`RST_DEVICES_L` is 04D102CA here and was transcribed as 049102CA from the T1 v2 photograph. The difference is bit 22 (USBD in
hekate's names). Three photographs here agree, so either T1 v2's transcription was wrong or the state differs. Unresolved;
`ENB_L` is identical.

## The writer: a candidate that was missing

On Erista, `launch_l4t` runs `hw_deinit` and then jumps to `BPMPFW_ENTRYPOINT`; hekate's own comment says "BPMP-FW runs BL31"
and the loader prints "Does power management". So NVIDIA's `bpmpfw.bin` runs on the BPMP after hekate's last CPU-side code and
before BL31 and U-Boot. Earlier notes (`ERISTA_T1_0507.md`, `ERISTA_T1R_DESIGN.md`) looked at hekate, BL31 and U-Boot and
found no writer; this one was not on the list. `bpmpfw.bin` is not in this repository and has not been read
(`boot/diag/README.md`); its hash is recorded there.

What points at a writer that programs the clock sources:

- Hekate writes only I2C1 and I2C5 (`hw_init`); nothing in its tree initializes I2C2, 3, 4 or 6. Both of its buses read
  `00000003`, neither `C0000003` nor `C0000000`.
- I2C5 reads divider 3 where hekate writes 0, so a mask that clears only the source field does not produce it.
- T1 v2 read the PWM source as `C0000004`, hekate's own word, so not every hekate-written source register was overwritten.
  That reading is from T1 v2, not from this run.

This is a pattern, not an identification.

## BL31 and U-Boot, by disassembly

A script follows constants within each function of the pinned images and resolves accesses through them (`mov`/`movk` bases,
`add`, immediate offsets). It does not follow values across calls or through a register offset, so it can miss writes.

- BL31: no resolved access to the six source offsets (0x124, 0x128, 0x198, 0x1B8, 0x3C4, 0x65C). Resolved CAR stores are
  `SCLK_BURST_POLICY`, `CLK_SYSTEM_RATE`, `RST_DEVICES_X`/`RST_DEV_X_SET`, `RST_DEV_Y_SET`, `RST_DEV_L_SET`/`H_SET`/`U_SET`/`V_SET`,
  `RST_DEVICES_V/W`, `CLK_OUT_ENB_V/W`, `CLK_SOURCE_SE`, `RST_CPUG_CMPLX_CLR`, and two offsets (0x74, 0x78) hekate does not
  name. There are none to I2C blocks. BL31 writes reset-set and clear registers, which can reset I2C1 (bit 12 of `RST_DEV_L`)
  but do not set a source.
- U-Boot: resolved CAR stores are 0x60006000, 0x88, 0x8C, 0xA4, 0xA8, 0xDC; none is a source offset. The earlier reading
  stands: the source setter has three call sites, and the control FDT enables I2C5 only. A U-Boot I2C5 probe could explain I2C5
  reading 3; nothing in U-Boot explains I2C1.

## PLLP

Per hekate's names, bit 30 (enable) and bit 27 (lock) of `PLLP_BASE` are set and bypass (31) is clear. Read with the Linux
field layout (M 7:0, N 15:8, P from bit 20) the word gives M 8, N 84, P 1. That does not reproduce 408 MHz for any reference I
can justify, so the layout or the assumption is wrong and no frequency is stated. Hekate's PWM table comment says HOS runs
PLLP / 54 at 7.55 MHz, which implies 408 MHz in HOS; that is another program's state. `PLLP_OUTA/B` are not decoded.

## The NOACK hypothesis, again

Still consistent: the I2C1 source differs from hekate's, hekate's divider fields were set for a 19.2 MHz source, and the
controller's own divisor (`DIV 00050001`) is unchanged. If the new source is the fast PLLP clock the bus would run far above
100 kHz. That needs three things nobody here has confirmed: PLLP's frequency, that field 0 selects PLLP on silicon, and the
TMP451 failing at that speed.

Weaker: that the cause is something specific to I2C1. Not excluded: the TMP451 not answering in shutdown, pad state, or a
different cause altogether. T1-R cannot tell these apart. The NOACK and the clock mismatch are two facts, not a proven link.

## Getting hekate's state just before the hand-off

| Option | Shows | Cost |
| --- | --- | --- |
| Read `bpmpfw.bin` offline (it is on the SD; hash in `boot/diag/README.md`) | whether it writes CAR source registers, with what | none on hardware; needs the file |
| A read-only payload from Nyx "Payloads" printing the six words | hekate's state after `hw_deinit` (payloads run it too), before BPMP-FW, BL31 and U-Boot | a new ARM payload; a new class of artifact |
| A patched hekate with a print | the same, in the l4t path itself | changes the bootloader |
| A BL31 stub that reads at EL3 | the state after BPMP-FW | replaces a pinned binary; most work |

Order: the offline read first, since it costs nothing and tests the one new candidate. If a payload is wanted afterwards,
`C0000003` there and 0 at P1 puts the writer in BPMP-FW, BL31 or U-Boot.

## Not done

No clock change. T1-R not re-run, T1 v3, T2 to T4 and D1 hardware boot stay on hold. `boot` stays 0.
