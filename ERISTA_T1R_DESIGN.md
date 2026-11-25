# T1-R: clock diagnostic, design

Design and research only. Nothing is implemented, built or run. T1 v2, D0, D0-R and D1 artifacts stay as they are. T1 is not
re-run, T2 to T4 and D1 hardware boot stay on hold, and `boot` stays 0. Why this exists: T1 v2 read `CLK_SOURCE_I2C1` as
`00000003`, hekate writes `C0000003`, and the first transfer got no ACK (`ERISTA_T1_0507.md`).

## What the sources say

| Question | Answer | Source | Status |
| --- | --- | --- | --- |
| I2C source mux | Field is bits 31:30 (2 bits). Value 0 = `pll_p`, value 3 = `clk_m`. hekate's `6 << 29` is `0xC0000000`, bits 31:30 = 3, so `clk_m`. The loader's 0 is `pll_p`. | Linux `clk-tegra-periph.c`: `mux_pllp_clkm` with index table `{0, 3}`, `I2C()` macro with shift 30, `MASK(2)` | Linux only. No Tegra210 TRM found. |
| I2C source divider | 16 bits, integer (no fractional part), rounded up: rate = parent / (field + 1). hekate's comment (19.2 MHz to 4.8 MHz at field 3) agrees. | Linux `I2C()` macro (`TEGRA_DIVIDER_ROUND_UP`, width 16, fractional width 0); hekate `clock.c` | Linux and hekate |
| Bus rate | `SCL = source / ((TLOW + THIGH + 2) * (std_fast_divisor + 1))`. Linux uses TLOW 4, THIGH 2 for standard mode; hekate's divisor field is 5 (`DIV 0x50001`, bits 31:16). | Linux `i2c-tegra.c` (`clk_multiplier`), hekate `i2c.c` | The timing registers' values at the loader are not read yet. |
| Estimate for the loader's state | 408 MHz / 4 = 102 MHz; 102 MHz / 8 / 6 = 2.125 MHz, against 100 kHz with hekate's value. | Arithmetic, with PLLP at 408 MHz assumed (Linux nominal, hekate sets SCLK from PLLP_OUT at 408 MHz) | **Assumption**: PLLP's rate, the mux meaning on this chip and the timing registers are not measured. |
| TMP451 speed limits | Fast mode up to 0.4 MHz. High-speed mode up to 2.5 MHz, and only after the master sends the high-speed master code byte (the part does not acknowledge it and then switches its SDA and SCL input filters). SCL low and high minimums 1300 ns and 600 ns in fast mode. The datasheet states no 3.4 MHz mode. | TI TMP451 datasheet SBOS686A, sections 6.6 and 7.5.1.6 | Read from the TI datasheet page. A plain 2.1 MHz transfer without the master code is outside fast mode. |
| TMP451 in shutdown | Shutdown stops "all device circuitry other than the serial interface". The datasheet does not say whether the part acknowledges in shutdown; one table row ("serial bus active, shutdown mode") implies the bus works. | Same, section 7.4.1 | ACK in shutdown is not stated. Not shown either way. |
| TMP451 SMBus timeout | Off by default; when on, the interface resets after SCL or SDA is held low 25 ms. | Same, section 7.5.1.5 | |
| Bit meanings | `INT_STATUS`: bit 2 arbitration lost, bit 3 NO_ACK, bit 7 packet complete, bit 11 bus clear done (Linux); bits 0, 1, 4, 5, 6 as hekate names them. `CNFG`: bit 10 packet mode, 11 new master FSM, 14:12 debounce (Linux); bits 0, 3:1, 6, 9 from hekate (A_MOD, length, command 1 read, normal-mode go). `CONFIG_LOAD`: bit 0 master load (both), bit 2 timeout load (hekate), bit 5 unknown. | Linux `i2c-tegra.c`, hekate `i2c.c` | **`STATUS` bits 3:0 (NOACK per byte) and bit 8 (BUSY) are defined only by hekate. Linux does not define `STATUS`. No TRM.** |

## Pinned U-Boot: can it write `CLK_SOURCE_I2C1`?

- **Address mapping is correct.** A periph-id to source-register function (`0xAA0034C0`) gives `0x60006000 + 0x100 + 4 * index`.
  I2C1 (id 12) has index 9, so 0x124; I2C5 (id 47) index 10, so 0x128. Both agree with Linux.
- **Who calls the source setter.** `0xAA002AE0` and `0xAA002B3C` (source and divider) have three call sites: two in the clock
  driver's `set_rate` and `get_rate` ops (`0xAA01B3B8`, `0xAA01B3F8`) and one in the Tegra I2C driver's controller init
  (`0xAA0215BC`, `0xAA021630`, which compares the requested frequency with 400000).
- **The control FDT** inside `bl33.bin` (offset `0x5B060`, 18052 bytes): `i2c@7000c000` (I2C1) is `disabled`;
  `i2c@7000d000` (I2C5) is `okay` at 400000 Hz with clock `<car 0x2f>`; I2C2, I2C3, I2C4 and I2C6 are `disabled`. So a normal
  probe reaches I2C5's register (0x128), not I2C1's.
- **No other path found.** `clock_early_init`-style code at `0xAA003788` onward writes PLLP_OUTA and PLLP_OUTB (0xA4, 0xA8) and
  a few mask registers (0x88, 0x8C, 0xDC); no I2C source register. Static reading only, not run; a call of `set_rate` for
  clock 12 from some other consumer is not excluded.
- **Correction.** An earlier note said `bl33.bin` held the CAR base once and `bl31.bin` not at all. That search looked for a
  32-bit literal; the code builds `0x60006000` as an instruction immediate (`mov w4, #0x60006000`). BL31 has such constructions
  too (a UART setup function at `0x3BCC` and about 89 `0x6000` and `0x7000` immediates in total, not reviewed one by one). The
  BL31 and U-Boot searches therefore do not support "nothing writes it"; they only found no U-Boot path to I2C1.
- **hekate at hand-off.** hekate writes the I2C1 clock once (`hw_init`, `clock_enable_i2c(I2C_1)`) and has no call to
  `clock_disable_i2c`. `hw_deinit` does not touch the I2C1 clock or its source; `tmp451_end` and `fan_set_duty(0)` are
  transfers and PWM, and the l4t launch code has no I2C clock access. By source reading, hekate keeps its value up to BL31.

So no writer is identified. U-Boot is a less likely candidate than before (I2C1 is disabled in its control FDT); BL31 is
unreviewed; hekate's own later code was not found to write it. A measurement is the way forward.

## T1-R

Independent entry, `iOS Diag T1R` (ident `iDiagT1R`, directory `switchroot/ios-diag-t1r/`, ini `ios-diag-t1r.ini`, Bazel target
`//boot:diag_t1r`, flag `SWITCH_DIAG_T1R`), built from new files `t1r*.c` and its own script `u-boot-script-t1r.txt`. D0's
script, `exit.c` and all other shared sources stay unchanged; the flag changes `diag.c` only when set, and the build must show
that the D0, D0-R, D1 and T1 v2 loader and script images are byte for byte unchanged (as was done for T1). Started from Nyx
"More configs" like the others. Success is a screen of values, nothing else.

### Where it reads

| Point | Reader | When | Registers |
| --- | --- | --- | --- |
| P1 | U-Boot script, `md.l` | after the DTB is copied, before `bootm start` | CAR set below, U-Boot side |
| P2 | U-Boot script, `md.l` | after `bootm prep`, before `bootm go`; then a 5 s pause so the lines can be filmed | same |
| P3 | the loader | at EL1, after its entry checks | CAR set and the I2C1 set below, loader side |

`md.l` has already read 27 MC registers on this console (`erista-first-run.log`), so the command exists and works; each line
prints address and value. The loader clears the screen at the hand-off, so P1 and P2 are read from the video or photographs.
P1 and P2 together show whether `bootm` changes anything; P3 is what T1 v2 saw. Hekate's own value cannot be read from this
chain; it could be separated only by a payload that reads before BL31, which is out of scope.

### Registers

All reads. Plain read, one 32-bit load, no read-to-clear register in the list.

| Register | Address | Read where | Condition | Side effect of a read |
| --- | --- | --- | --- | --- |
| `RST_DEVICES_L` | 0x60006004 | P1, P2, P3 | none (CAR is always accessible) | none known |
| `CLK_OUT_ENB_L` | 0x60006010 | P1, P2, P3 | none | none known |
| `CLK_SOURCE_I2C1` | 0x60006124 | P1, P2, P3 | none | none known |
| `CLK_SOURCE_I2C5` | 0x60006128 | P1, P2, P3 | none | none known; U-Boot's own bus, for contrast |
| `CLK_SOURCE_I2C2`, `I2C3`, `I2C4`, `I2C6` | 0x60006198, 0x600061B8, 0x600063C4, 0x6000665C | P3 | none | none known; which buses still carry hekate's value |
| `PLLP_BASE`, `PLLP_OUTA`, `PLLP_OUTB` | 0x600060A0, 0x600060A4, 0x600060A8 | P1, P2 (BASE only), P3 | none (U-Boot reads 0xA0 itself) | none known. Raw words only, not decoded: the field layout has no source here. |
| I2C1 `CNFG`, `STATUS`, `INT_STATUS`, `CLK_DIVISOR`, `CONFIG_LOAD` | 0x7000C000 + 0x00, 0x1C, 0x68, 0x6C, 0x8C | P3 | CAR says I2C1 clock on and out of reset, else skipped | none known (T1 v2 read the same five) |
| I2C1 `INTERFACE_TIMING_0`, `_1` | 0x7000C094, 0x7000C098 | P3 | same | Linux offsets; reset values and read side effects not documented in any source read. Plain configuration registers. |

Not read: `CMD_DATA1` and the FIFOs (a data read may pop a byte), `BUS_CLEAR_STATUS` (hekate reads it for no stated reason, so
a side effect is possible), any pad or GPIO. Not written: anything.

### Forbidden

I2C transfers of any kind (no address or data write, no `CONFIG_LOAD` write, no go), clock or divider writes, bus clear or FIFO
flush, any TMP451 access or setting, fan, PWM, voltage. The loader has no MMIO store function at all: the inspection requires
`switch_t1r_read32` to be a single load and rejects any other device store in the image. The U-Boot script may contain `md.l`
and the D0 commands, and no `mw`, `i2c`, `mm`, `nm`, `setenv` of a hardware value or `fdt` change beyond D0's.

### Screen

CAR block: `RST_L`, `ENB_L`, each with the I2C1 and PWM bits spelled out. I2C table: six rows (I2C1 to I2C6), the raw word, the
source field (bits 31:30) and the divisor field (bits 15:0), with the note "hekate writes C0000003 for I2C1, C0000000 for
I2C5; mux 0 pll_p, 3 clk_m per Linux". PLLP words raw. I2C1 controller words and the two timing words, or a line saying why
they were not read. No estimate of SCL on the screen: it needs PLLP's rate and the timing registers, which the raw words only
suggest. The estimate stays in the review, with its assumption.

### Reading the result

| Observation | Meaning |
| --- | --- |
| I2C1 is `C0000003` at P1 and P2 but `00000003` at P3 | something between `bootm prep` and the loader's first read changed it (the hand-off or the entry code) |
| `00000003` at P1 already | changed before the script: after hekate's `hw_init`, in BL31, or in U-Boot's start. Not separable from this chain. |
| `00000003` at P1, P2 and P3, I2C5 changed to a pll_p value | U-Boot's I2C driver ran for I2C5 and the clock code set a source; I2C1's partial change is then still unexplained |
| `C0000003` at all three | T1 v2's reading does not reproduce; compare the start path and repeat once |
| I2C2 to I2C6 hold hekate's values | the change is specific to I2C1 |
| Timing registers not at 4 and 2 | the SCL estimate changes; recompute |

## Run limits

Adds a 5 s pause and ten reads. Proposed total deadline 50 s from selecting the entry (T1's 45 s plus the pause), with the same
stage deadlines and the same stop conditions and recovery check as `ERISTA_T1_RUN.md`. The real time of the T1 0507 run (from
the video, selection to hekate's menu) should replace the estimate before this is set; it was not recorded. A hung read of an
unclocked I2C1 register is guarded by the CAR check, as in T1; the CAR, PLLP and I2C1 reads in the U-Boot script cannot hang on
the CAR (always clocked). The thermal position is unchanged from T1: no load added, fan off, temperature not measured.

## T1 v3 (not designed here)

Before any transfer, T1 v3 stops when `CLK_SOURCE_I2C1` is not hekate's `C0000003` (a new stop code), with the value on screen.
It is a guard, not a fix, and waits for T1-R's result.

## Review points

1. Whether P1 and P2 both are needed, or P1 alone with P3.
2. Whether to read the two timing registers (undocumented side effects) or leave them for a second step.
3. The pause length and the deadline, once the T1 run time is known.
4. Whether `md.l` output is the right way to see P1 and P2, or the loader should receive them (a RAM write; not proposed).
