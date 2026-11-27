# T3-R: read the fan state at the handoff

Design of 2026-11-27, implemented the same day as code and offline checks only (`ERISTA_T3R_RUN.md`). Nothing is approved or run. T3-R is a read-only diagnostic: no store to a device address, no fan, no rail, no pinmux or GPIO change. `boot` stays 0, the feature mask 0, T3 and T4 are not waived, T4-A1, T4-A2 and T4-P are not implemented, the first XNU run is on HOLD, the ten existing diagnostic hashes stay as they are. A run needs its own written approval. T4 work is paused.

Marks: **confirmed** (read in the file or run named), **inferred**, **unknown**. hekate v6.5.5 files were read through a fetch tool that summarises, so quotes are close and not byte-checked.

## What T3-R answers

`ERISTA_THERMAL_DESIGN.md` writes T3's order assuming a starting state. T3-R reads that state on this console, at the point where our loader runs, so T3 can be reviewed against a measured start and not against hekate's code alone. It does not tell whether the fan turns.

## What hekate does on an Erista

From `bdk/thermal/fan.c` and `bdk/power/regulator_5v.c` at v6.5.5.

| Step | Code | Erista difference |
| --- | --- | --- |
| First call of `fan_set_duty` | `CAM1_PWDN` = tristate, input enable, pull up, function 1; GPIO S7 input; `PWM_CSR_1` = `EN \| (0x100 << 16)`; `LCD_GPIO2` = 1; GPIO V4 to SPIO | the pull up only when the chip is a T210; the PWM clock is enabled only on an Aula, so on an Erista it relies on what hekate left on |
| Off (duty 0) | `PWM_CSR_1` = `EN \| (0x100 << 16)`; 5 V fan rail off; `LCD_GPIO2` = input enable, parked, tristate, pull down | |
| On | 5 V rail on; `PWM_CSR_1` = `EN \| ((236 - duty) << 16)`; `LCD_GPIO2` = 1 | the output is inverted |
| 5 V rail on | `SATA_LED_ACTIVE` = 1; GPIO A5 output high; on a T210 `USB_VBUS_EN0` = `LPDR \| 1` and GPIO CC4 output low; PMC `NO_IOPOWER` bit 21 cleared; PMC `PWR_DET_VAL` bit 21 cleared | the CC4 and `USB_VBUS_EN0` part is T210 only (the comment says only Icosa has USB 5 V rails) |
| 5 V rail off | GPIO A5 low; on a T210 GPIO CC4 low; no pinmux or PMC write | |
| rpm | poll GPIO S7 for 2 s, halve the edges, times 30 | a polled count loses edges when fast |

The bit values come from hekate's headers: `PINMUX_TRISTATE` bit 4, `PINMUX_PARKED` bit 5, `PINMUX_INPUT_ENABLE` bit 6, `PINMUX_LPDR` bit 8, pull down `1 << 2`, pull up `2 << 2`, `PWM_CSR_EN` bit 31. **Confirmed** at the file level; not byte-checked.

What hekate's off state is then, by the code (not seen on this console): `PWM_CSR_1` `0x81000000`, `LCD_GPIO2` `0x74`, A5 and CC4 low, `NO_IOPOWER` bit 21 and `PWR_DET_VAL` bit 21 left as the enable path last wrote them. The off word carries `PWM_CSR_EN` and, in the `0x100 << 16` field, bit 24 (the PWM block's "absolute 0 percent", the same bit `fan_set_duty` writes for duty 0). The duty counter in `fan.c` starts at -1, so the first call always runs the initialisation before either branch; that settles the order question, and `CAM1_PWDN` `0x59` follows if Nyx ran `hw_deinit` at all. Only `BDK_HW_EXTRA_DEINIT` in Nyx calls it, so the prediction assumes Nyx did. T3-R compares to these and treats a difference as information, not as failure. First run (2026-11-27, `ERISTA_T3R_RESULT.md`): `LCD_GPIO2` and `CAM1_PWDN` matched, `PWM_CSR_1` read `0x00000000`, so the off-word prediction was wrong for this launch path.

Cross-check against hekate v6.5.5 (2026-11-27): no contradiction with the code was found, so nothing in the implementation is a guess. Register values, bit positions and the duty formula are the ones in the tables here. A caveat stays: mainline-style PWM documentation describes an 8 bit duty field at bits 23:16 with no "absolute 0" bit. Whether bit 24 means that on this block cannot be settled without the TRM. T3-R therefore reports bit 24 only as part of the one exact off word and calls every other word with it set UNKNOWN.

## What is reused

No new CAR read is needed. Measured at the loader, all as 32 bit words:

| Register | Address | Value | Runs |
| --- | --- | --- | --- |
| `RST_DEVICES_L` | `0x60006004` | bit 17 (PWM) clear in `049102CA` and `04D102CA` | D0-S v2, T1-R (`ERISTA_D0S_V2_RESULT.md`, `ERISTA_T1R_RESULT.md`) |
| `CLK_OUT_ENB_L` | `0x60006010` | bit 17 set in `9802D131` | the same, and T1 v3, T1-T, T2 (`ERISTA_T3T4_REUSE.md`) |
| `TMRUS` | `0x60005010` | counts in microseconds | T4-R (`ERISTA_T4R_RESULT.md`) |
| pinmux pads `0x700030BC`, `0x700030C0` | I2C1 SCL and SDA pad words | `00000040` | T1 (`ERISTA_T1.md`, `ERISTA_T1_0507.md`, `boot/diag/loader/t1_regs.c`) |

The two words of `RST_DEVICES_L` differ in a bit other than 17 between runs. That does not affect the PWM gate, and it is a reason to read the gate registers again in the T3-R run itself, not to rely on the earlier value. Both are addresses already in use by earlier diagnostics, so they are gate reads, not new addresses.

## New reads

All 32 bit, no side effect known. Plain registers of these blocks have no read-to-clear behaviour in any source read; hekate itself reads GPIO and PMC words back after writes (`(void)GPIO(offset)`). **Inferred**, not shown for this console.

| # | Register | Address | Basis | Why |
| --- | --- | --- | --- | --- |
| 1 | `PWM_CSR_1` | `0x7000A010` (`PWM_BASE` `0x7000A000`, offset `0x10`) | `t210.h` | is the fan off, which duty |
| 2 | pinmux `LCD_GPIO2` | `0x7000320C` | `PINMUX_AUX_BASE` `0x70003000` plus `0x20C` | the fan signal pin: function 1 or parked |
| 3 | pinmux `CAM1_PWDN` | `0x700031EC` | plus `0x1EC` | the tachometer pin: pull up |
| 4 | pinmux `SATA_LED_ACTIVE` | `0x7000304C` | plus `0x4C` | 5 V rail enable pin |
| 5 | pinmux `USB_VBUS_EN0` | `0x700031A8` | plus `0x1A8` | the T210 rail pin |
| 6 to 8 | GPIO A (A5) `CNF`, `OE`, `OUT` | `0x6000D000`, `0x6000D010`, `0x6000D020` | `GPIO_BASE` `0x6000D000`; bank = port >> 2 (0x100 each), port offset = (port % 4) * 4 | rail enable: is A5 a GPIO, output, high |
| 9 to 11 | GPIO CC (CC4) `CNF`, `OE`, `OUT` | `0x6000D700`, `0x6000D710`, `0x6000D720` | port 28: bank 7, offset 0 | USB VBUS rail |
| 12 to 14 | GPIO S (S7) `CNF`, `OE`, `IN` | `0x6000D408`, `0x6000D418`, `0x6000D438` | port 18: bank 4, port 2 (offset 8) | tachometer: input, and its level once |
| 15, 16 | GPIO V (V4) `CNF`, `OE` | `0x6000D504`, `0x6000D514` | port 21: bank 5, port 1 (offset 4) | the fan power-mode pin: SPIO or GPIO |
| 17 | PMC `NO_IOPOWER` | `0x7000E444` | `PMC_BASE` `0x7000E400`, `0x44`; bit 21 `GPIO` | rail enable also clears it |
| 18 | PMC `PWR_DET_VAL` | `0x7000E4E4` | `0xE4`; bit 21 `33V_GPIO` | the same |

Eighteen new reads, plus `TMRUS` at the start and the end and the two CAR gate words, 22 reads in all. The GPIO addresses follow hekate's `GPIO_PORT_OFFSET` macros; the GPIO base is `t210.h`'s. The `IN` read of S7 is one sample of a level, not an edge count.

Dropped: `OUT` of S and V, `IN` of A, CC and V (nothing the design needs), every write, every edge count.

## Risks of the reads themselves

- A gated block can hang the bus on a read. The PWM block is gated by CAR; the gate words decide row 1 and nothing else reads PWM.
- Pinmux pad reads have a precedent (T1: the two I2C1 pads answered at EL1). GPIO and PMC reads have none in this repository. Whether a non-secure EL1 read of a secure-protected word returns zero, all ones or aborts is **unknown**; T4-R's TKE reads at EL1 answered, which says nothing about these blocks. The PMC rows are last because they carry the most secure-access doubt.
- A read that never completes cannot be left by software. Each step is drawn and settled before it is made, so the screen names the last one begun; the person holds POWER (the repository's documents cite Nintendo's 12 s; not tried here, not treated as a certain recovery).

## Order

1. `TMRUS` (start).
2. CAR `RST_DEVICES_L`, `CLK_OUT_ENB_L`. The PWM gate is open only when `CLK_OUT_ENB_L` bit 17 is set and `RST_DEVICES_L` bit 17 is clear. Otherwise the PWM read is shown as `NOT READ` with the reason and skipped; the run goes on, since no other block depends on PWM.
3. `PWM_CSR_1` if the gate is open.
4. Pinmux: `LCD_GPIO2`, `CAM1_PWDN`, `SATA_LED_ACTIVE`, `USB_VBUS_EN0`.
5. GPIO in the order A, CC, S, V.
6. PMC `NO_IOPOWER`, `PWR_DET_VAL` last.
7. `TMRUS` (end), the FAN, ROUTE, RAIL and TACH lines, COMPLETE, the timed exit.

Each read is a BEGIN line, the read, and a RESULT line. Each line is drawn and then settles 100 ms before the next step runs.

## Time limits

Two limits exist in code, one is a person's rule, and the overall figure is provisional. None is a safety guarantee, and none is measured on this image.

| Limit | Value | Where |
| --- | --- | --- |
| One shown step | 100 ms at 19.2 MHz; a counter at another frequency counts as not settled and stops the run (`0x902`) | `t3r_settle.c` |
| All reads | 20 s from the start of the run, checked before every read; spent means stop `0x903` | `t3r_settle.c`, `t3r.c` |
| Dwell and reset | 15 s `wfe`, then PSCI `SYSTEM_RESET` | `switch_diag_exit` |
| No new line | 30 s: the person holds POWER | on the screen |
| From selection to hekate's menu | provisional 60 s; later than that, hold POWER | the run sheet |

The arithmetic: about 50 shown steps at 100 ms is about 5 s of reads, plus the 15 s dwell, plus a return to hekate that no run has timed (about 6 s is an assumption, not a figure). That is about 26 s, so 60 s is more than twice the expectation. A read that hangs the bus cannot be left by any of this: the 20 s budget is only checked between reads.

## Stop conditions

The run ends at once, shows COMPLETE with the stop code, and goes to the exit when:
- the allowlist refuses a register or width (`0x901`, a loader bug);
- a line did not settle, or the counter is not 19.2 MHz (`0x902`);
- the read budget is spent before the next read (`0x903`);
- a word reads as all ones (`0x904`); `TMRUS` is exempt because a counter may read any value.

Not stopping: a value that differs from hekate's off state. It is printed with its prediction.

## PWM classification

`PWM_CSR_1` is classified by what hekate writes, and nothing else is called known:

| Word | State |
| --- | --- |
| not read (gate closed) | NOT CLASSIFIED |
| `0` | DISABLED |
| `0x81000000` | OFF AS HEKATE |
| bit 31 set, bit 24 clear, bits 30:25 and 15:0 clear, field 0 to 235 | DUTY `236 - field` |
| bit 31 set, bit 24 clear, field 236 | UNKNOWN, special zero duty (hekate never writes it) |
| bit 31 set, bit 24 clear, field 237 to 255 | UNKNOWN, out of range |
| bit 31 clear and any other bit | UNKNOWN, enable clear with bits |
| bit 24 set with any bit besides 31 and 24 | UNKNOWN, absolute 0 with bits |
| bits 30:25 or 15:0 set | UNKNOWN, other bits |
| `0xFFFFFFFF` | UNKNOWN, all ones (also a stop) |

A DUTY line is a reading of hekate's formula, not a statement that a fan turns.

## Screen

1280 by 720, two columns of 50 character cells. Reads 1 to 11 are in the left column at rows 9 to 19, reads 12 to 22 in the right at the same rows; a BEGIN line is overwritten in place by its RESULT line. The glyph set has no comma and no plus, so lines use letters, digits and `: - . = / ( ) _ ?`. Rows 20 to 25 hold CAR (both raw words and the gate), FAN, ROUTE, RAIL, TACH and COMPLETE, each the full width; rows 26 to 29 the person's rules and the notes (the reads end by themselves after 20 s; a hung read is not left by a timeout; the state is after hekate and U-Boot; GPIO and PMC reads at non-secure EL1 are untested); row 30 `BOOT STAYS 0`; the exit status from row 31.

- CAR: `RST_DEVICES_L` and `CLK_OUT_ENB_L` as read, bit 17 of each, gate open or closed.
- FAN: the PWM word, its state, the duty or the reason it is UNKNOWN.
- ROUTE: `LCD_GPIO2` against `0x74`, `CAM1_PWDN` against `0x59`.
- RAIL: A5 and CC4 `OUT` bits, the two rail pinmux words, the two PMC words.
- TACH: S7 `CNF`, `OE` and the one `IN` sample.
- COMPLETE: reads made, stop code, `NO WRITES`.

An Erista gate runs first; on another board it prints `NOT AN ERISTA. NO T3R REGISTER TOUCHED`. The nine record words 55 to 63 keep the stop code, the read count and the raw words for a later photograph.

## Exit

The shared exit of every diagnostic (`switch_diag_exit`, `boot/diag/loader/exit.c`): `CurrentEL` check, counter check, `RESET IN 15 S`, an event stream and `wfe` until the deadline, then PSCI `SYSTEM_RESET` (`0x84000009`). **Confirmed** in eight runs that ended in hekate (D0, D0-R, T1-T, T1 v3, D0-S v2, D1-min, T2 and T4-R). On a failed gate it prints `HOLD THE POWER BUTTON` and halts, as now. T3-R adds nothing to this path.

## Timing basis, independent of the first XNU run

The T4-A design takes its period from "video timings of the first run". If that were the first XNU run, T4-A could not precede it. The basis is changed here to the independent diagnostics:

- The T3-R run's video, with a clock in frame, gives: selection to the first line; the interval between lines; the line `RESET IN 15 S` to hekate's menu. The last figure is the reset dwell plus the return time, which no run has recorded (T4-R's video was not taken).
- A POWER hold timed from a running hekate, and the RCM jig timed from off to injection (the owner's rehearsal in `ERISTA_T4A_DESIGN.md`), give the recovery times.
- None of these is a measurement of XNU. Loader-side times bound the loader's exit; the XNU stop rules (30 s, `n + 10` s) keep their provisional status until the first XNU run's own video, and that video is no longer a prerequisite of T4-A.

## Boundary with the fan run

| | T3-R | T3 (fan rotation) |
| --- | --- | --- |
| Stores to a device | none | pinmux, GPIO, PMC and PWM writes |
| Fan | untouched | on at the lowest duty, then off |
| 5 V rail | read | switched, and it shares USB VBUS handling (CC4) |
| Evidence | the state at the handoff | edges counted on S7 with the rail off and on |
| Allowlist | reads of the table above | the writes as well; none exist |
| Approval | its own, after review | its own, after T3-R's result and a review of the write list |

T3-R's result is an input to T3: if the fan is already on, or the rail is on, or the PWM clock is off, T3's order and its off sequence change before anything is written. A single `IN` sample cannot show a fan turning; only T3's edge count can.

## Implementation

`boot/diag/loader/t3r*.c` (state machine, register table, gate, report, settle, access), image `//boot:diag_t3r`, entry `ios-diag-t3r`, hekate name `iOS Diag T3R`, define `SWITCH_DIAG_T3R`. The one device-read function is the only place an address is used; the audit asserts the 21 address words appear once each and no store to a device address exists. Checks: `boot/diag/loader/test/t3r_test.cpp`, `tools/package/test/diag_t3r_test.py`. The ten existing images keep their hashes; `diag_images` reports eleven inspections.

## Open

- Whether non-secure EL1 reads of GPIO and PMC words answer (only T3-R shows).
- Whether Nyx ran `hw_deinit` before our loader on this launch path (decides the `CAM1_PWDN` prediction).
- Whether bit 24 of `PWM_CSR_1` is "absolute 0 percent" on this block (the TRM).
- The fan's rpm range and tachometer edge rate (a running Linux, or T3).

## Sources

`ERISTA_THERMAL_DESIGN.md`, `ERISTA_T3T4_REUSE.md`, `ERISTA_T3T4_MINIMAL.md`, `ERISTA_T4R_DESIGN.md`, `ERISTA_T4R_RESULT.md`, `ERISTA_T4R_RUN.md`, `ERISTA_D0S_V2_RESULT.md`, `ERISTA_T1R_RESULT.md`, `ERISTA_T4A_DESIGN.md`; hekate v6.5.5 `bdk/thermal/fan.c`, `bdk/power/regulator_5v.c`, `bdk/soc/gpio.c`, `gpio.h`, `pinmux.h`, `t210.h`, `pmc_t210.h`; `boot/diag/loader/exit.c`, `t4r*.c`, `t1_regs.c`.
