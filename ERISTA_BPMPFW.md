# bpmpfw.bin (Erista) read offline

The hekate 6.5.5 release `bootloader/sys/l4t/bpmpfw.bin`, copied from the experiment card. SHA-256 equals the recorded
`943463950880ae7892a1eb8d3a280b3f86083bca0164c663bf4126210be73ecb` (`boot/diag/README.md`), size 101780 bytes. The file is
not stored in this repository. `tools/package/bpmpfw_scan.py` reads it, `boot/diag/erista-bpmpfw-scan.txt` keeps the
derived accesses, and `tools/package/test/bpmpfw_scan_test.py` tests both. Nothing was run on hardware.

## Confirmed from the binary

- Header: bytes 0 to 3 `bpmp`, then the words 0, 0x21, 0x1FC00 and 0x100. Bytes 0x90 to 0xFF hold 64 hex digits as ASCII.
  Code starts at 0x100.
- The code is 32-bit ARM instructions. The entry at 0x100 computes its own load address (`sub r9, pc, #8` minus 0x100), so it
  runs from wherever it is loaded.
- Entry copies two segments to IRAM: file 0x268 to 0x40000400 (end 0x4000C6F0), and file 0xC558 to 0x4000C6F0 (end
  0x4001C828). The second copy is longer than the rest of the file by 14588 bytes. A flag at 0x4000C73C picks a cold path
  (set the flag, clear 0x4000C840 to 0x4000EF3C, set up stacks, branch to 0x4000606C) or a warm path (copy two more
  segments, jump to 0x400078C8).
- The image is a small kernel (strings `common/kernel/thread.c`, `platform/drivers/mailbox/mail_t210.c`) with a mailbox
  driver for T210, a PMIC path on I2C5, and a power-state function that prints `bpmp:sc7 exit`.
- Erista against Mariko: there is no read of a chip or revision register in the image (no literal for 0x70000804 or any
  0x700008xx word). Hekate chooses the file: `bpmpfw.bin` for T210 and `bpmpfw_b01.bin` for T210B01
  (`bootloader/l4t/l4t.c`). The header word 0x21 is the T210 chip number, which fits but is not stated anywhere.
- 89 device accesses resolve (`erista-bpmpfw-scan.txt`): CAR, I2C5 and PMC. Not one address is any of the six
  `CLK_SOURCE_I2C` registers (0x124, 0x128, 0x198, 0x1B8, 0x3C4, 0x65C). Those numbers also do not appear as an
  instruction immediate, as a word in a literal pool, or as a halfword or byte in a table.
- I2C: only I2C5 (0x7000D000). Its clock function reads `CLK_OUT_ENB_H` bit 15 and, if the clock is off, sets
  `CLK_ENB_H_SET`, pulses `RST_DEV_H_SET` and `RST_DEV_H_CLR`. It never writes `CLK_SOURCE_I2C5`. The transfer function
  writes `CMD_ADDR0`, `CMD_DATA1`, `CNFG`, `CONFIG_LOAD` and polls `STATUS` bit 8. I2C1, 2, 3, 4 and 6 are never touched.
- PLLP: `PLLP_BASE` is written only through the small read-modify-write helper, in the power-state function at
  0x40008DD8: clear bits 31:30 (off), and clear bit 31 with bit 30 set (on), then up to 150 polls of bit 27 (lock), each after a
  wait of 2 timer units, and a wait of 10 after lock is seen (the unit is taken to be microseconds). Bit 18 of `PLLP_MISC` (offset 0xAC) is set. `PLLP_OUTA` bit 1 is
  cleared, and `PLLP_OUTA` and `PLLP_OUTB` are OR-ed with a value that is not a constant (a saved word, by inference). The divider fields M, N and P are never written.
- That function is called from the main loop at 0x40002474 and chooses among six states by a jump table, so it runs on a
  power-state request, not as part of start-up.
- The other CAR writes are reset and enable set and clear registers (H, V, X banks), `CCLK_BURST_POLICY`, `SCLK_BURST_POLICY`,
  `SUPER_SCLK_DIVIDER`, `CLK_SOURCE_EMC` and `CLK_SOURCE_EMC_DLL`, and two offsets (0x644, 0x671C) hekate does not name.

## Inferred

- The PLLP and burst-policy code is a low-power entry and exit sequence, because it sits next to the `sc7 exit` string.
- The cold path does not touch CAR beyond the V-bank enable and reset of bit 16 (ATOMICS in hekate's names) and PMC scratch
  words. The reading is by hand from the entry code and the straight-line scan.
- This build of BPMP-FW is not what rewrote `CLK_SOURCE_I2C1` and `CLK_SOURCE_I2C5`: it never addresses them.

## Not verified

- Anything reached only through a function pointer or a value stored in memory. Two PLLP and EMC functions (0x40008C30 and
  0x40008B80) and the I2C5 clock and transfer functions have no direct caller and are reached that way. The scan follows
  constants inside straight runs of code, so it can miss a write whose address comes from memory.
- What the 14588 bytes past the end of the file are when the second copy runs, and what the 64 hex digits mean.
- The CPU model (ARM7-class is expected, not shown) and the load address in practice (hekate defines `BPMPFW_BASE`).
- Whether BL31, U-Boot or something not yet read writes the two registers. This reading narrows the list; it does not name a
  writer.

## PLLP_BASE = 48115408 with the T210 layout

Linux `clk-tegra210.c` for T210 (source, not the reference manual): `pllp_nmp` is `divm` bits 7:0, `divn` bits 17:10 and
`divp` from bit 20, five bits. The table row for a 38.4 MHz input is `{ 38400000, 408000000, n 85, m 8, p 1 }`, and
`pll_p_params` has `fixed_rate = 408000000`, lock at bit 27, enable bit 30, bypass bit 31.

Decoded: M = 8, N = 85, P = 1, enable and lock set, bypass clear. That is the table row exactly, and 38.4 MHz / 8 x 85 =
408 MHz. My first decode used `divn` at bit 8, which was wrong and gave N = 84; it is replaced. The 38.4 MHz input is
supported by `CNTFRQ` 19.2 MHz (half the oscillator) and by hekate's comment that `clk_m` is 19.2 MHz.

Status: PLLP at 408 MHz is strongly supported (a three-field match to a table row), not confirmed against the manual.
`PLLP_OUTA` 00000003 and `PLLP_OUTB` 00030003 are still not decoded.

## What this does to the NOACK hypothesis

Linux maps I2C source 0 to `pll_p` and 3 to `clk_m` for all six buses; hekate's own comments say source 6 (the same bits) is
19.2 MHz `clk_m`, divided by 4 to 4.8 MHz for I2C1. The measured word 00000003 is then `pll_p` / 4 = 102 MHz, 21.25 times
hekate's 4.8 MHz. With the formula used before (module clock / ((TLOW + THIGH + 2) x (divisor + 1)), 4, 2 and 5, which
gives 100 kHz for hekate's clock), the bus would run near 2.1 MHz against the TMP451's 400 kHz fast-mode limit.

Still open: the I2C timing registers were not read, the formula is Linux's, and the TMP451 failing at that speed is not
shown. This strengthens the clock explanation and does not make it proven.

## Next, design only

Nothing here changes any clock, BPMP-FW or hardware state.

1. BL31 and U-Boot with the same helper-call resolver (the BL31 and U-Boot scan did not resolve helper calls).
2. A U-Boot probe path: which of its drivers can set an I2C source rate (`assigned-clocks` is absent from its control FDT).
3. The read-only payload from `ERISTA_T1R_RESULT.md`, once approved, to see the value before BPMP-FW runs.
