# T3-R: result of the first run

Run by the owner on 2026-11-27 (report 16:10 JST) with the image of commit `4a903d590`, one run, one photograph. Analysis only. T3-R is not run again, no fan or 5 V code is written, `boot` stays 0, the feature mask stays 0, T3 and T4 are not waived, the first XNU run is on HOLD.

Values below were read from the photograph and the owner's report. The photo is a rotated phone shot; digits I could not read twice are left out. Nothing was measured by this analysis.

## Three separate findings

| | Finding | Class |
| --- | --- | --- |
| 1 | The 22 reads were made, the loader stopped with code 0000 and hekate came back by itself | success |
| 2 | The last `TMRUS` line says `DID NOT ADVANCE` although the counter advanced | display bug in T3-R, not a timer fault |
| 3 | The summary lines overwrite the result lines of reads 2 to 6 and spill into the right column | screen layout bug in T3-R |
| 4 | `PWM_CSR_1` is `0x00000000`, not `0x81000000` | a result that differs from the prediction; a likely cause is inferred from the source (below) |

### 1. Reads and return

`EL1 STATE PASS`, `READS 22`, `STOP 0000`, `NO WRITES`. GPIO and PMC reads at non-secure EL1 answered, which closes the open item the run sheet carried. After the 15 s dwell and `SYSTEM_RESET` hekate returned with no hand action: the ninth confirmed return to hekate. Run time was given as about 15 to 18 s, an estimate. The reads alone took 4.25 s by `TMRUS` (below) and the dwell is 15 s, so the owner's figure was counted from a point after the start of the reads. Start temperature 25.1 C, end 27.9 C, +2.8 C (owner's readings, not a calibrated measurement).

### 2. `DID NOT ADVANCE`

Raw reads from the screen: first `TMRUS` `0x0680CFDD`, last `0x06C1B6A5`. Unsigned difference `0x06C1B6A5 - 0x0680CFDD = 0x0040E6C8 = 4,253,384`. That is 4.25 s if the counter counts microseconds. The design expects 42 settles of 100 ms between the two reads, 4.2 s, so the counter and the 19.2 MHz settle agree to about 1.3 percent. The counter advanced.

Cause, from the code and reproduced on the host with these two raw words:
- `switch_t3r_advanced` is correct: the unsigned delta is nonzero and below `0x80000000`.
- `step_read` stores the second reading into `result->tmrus_second` and then draws the RESULT line. That is why the displayed value is right.
- `result->tmrus_advanced` is computed only after `read_sequence` has finished the loop, which is after the last RESULT line was drawn. At that draw the flag is still zero from the clearing at the start of the run, and `result_line` prints `DID NOT ADVANCE` and marks the line.
- Host reproduction (not committed): at the draw of the last RESULT line `tmrus_second` is `06C1B6A5` and `advanced` is 0; after the run `advanced` is 1.
- Saving is not affected: the record word `D_T3R_TMRUS` packs the first and the second value directly, not the flag.

Why the tests missed it: `t3r_test.cpp` reads the line text from the final result structure, not from the structure the screen callback was given at draw time. T4-R's flag is ready before its draw, so T3-R's order was not covered by that precedent.

### 3. Overlapping summary lines

`fan_line`, `route_line`, `rail_line`, `tach_line` and the COMPLETE line call `place(out, SWITCH_T3R_COLUMN_ROWS + n)`. `place` reduces the index modulo 11, so n = 1 to 5 lands on rows 1 to 5 of the left column, where the results of reads 2 to 6 sit, not on rows 21 to 25 as the design says. The lines are longer than 50 cells, so they also run into the right column and print over the results of reads 14 to 16 (the screen shows the mixed text).

What this hides on the screen: the result lines of `RST_DEV_L`, `CLK_ENB_L`, `PWM_CSR_1`, `LCD_GPIO2`, `CAM1_PWDN` (their values are repeated in the FAN and ROUTE lines, except the two CAR words), and reads 14 to 16 (`GPIO_CC_OUT`, `GPIO_S_CNF`, `GPIO_S_OE`; their bits are in the RAIL and TACH lines). The two raw CAR words are on no screen line. They are in the record word `D_T3R_CAR` only, which was not reported. What is known about them: the FAN line printed a word, so the gate was open, so `CLK_OUT_ENB_L` bit 17 was set and `RST_DEVICES_L` bit 17 clear.

### 4. `PWM_CSR_1` is zero

Observed: `00000000`, shown as `DISABLED OUT OF RESET`. The enable bit and every other bit are clear. The classifier did its job: zero is a defined state (DISABLED), not UNKNOWN.

The follow-up reading of hekate v6.5.5 (2026-11-27, summarising fetch, quotes not byte-checked) gives a likely cause. The owner starts diagnostics from Nyx. Nyx's `_launch_hos` runs `hw_deinit(false)` with `BDK_HW_EXTRA_DEINIT`, whose `fan_set_duty(0)` writes `0x81000000`, and then jumps to hekate's `ipl_main`. `ipl_main` calls `display_backlight_pwm_init()`, which calls `clock_enable_pwm()`. `clock_enable` puts the PWM module into reset, enables the clock and releases the reset. A reset of the block clears `PWM_CSR_1` along with the backlight's `PWM_CSR_0`, which hekate then writes again. That ordering ends with `PWM_CSR_1` at zero, clock on and reset released, as seen. The first T3-R design did not follow `ipl_main` this far and predicted `0x81000000`. This is **inferred**: each step is in the source, nothing in the chain was observed on its own, and the reset value of the register is not confirmed (no TRM here). The pinned U-Boot `bl33.bin` has no PWM block literal (partial scan), so it is not the likely writer.

What stays unknown: the register's reset value, and whether anything else cleared it. The pinmux words `0x74` and `0x59` match what Nyx's deinit and fan init write, but may equal reset defaults too.

What it means for the fan work (`ERISTA_T3_FAN_DESIGN.md`):
- The state at our entry is `PWM_CSR_1` zero, not hekate's off word. A fan step writes the whole word itself and reads it back.
- A fan step must not touch the PWM clock or reset bits: a reset pulse would zero `PWM_CSR_1` and `PWM_CSR_0`, which holds the backlight.
- `LCD_GPIO2` is tristated, parked and pulled down, so the pad is not driving the fan line. That is an inference from the pad word. No fan speed, tach edge or 5 V voltage was measured, so "the fan is off" is not established.
- The one off-state word T3-R was built to confirm (`OFF AS HEKATE`) was not seen; the prediction table in `ERISTA_T3R_DESIGN.md` was wrong for this row.

## Other values

| Read | Value | Against the prediction |
| --- | --- | --- |
| `LCD_GPIO2` | `0x00000074` | as predicted |
| `CAM1_PWDN` | `0x00000059` | as predicted |
| `SATA_LED_ACTIVE` | `0x00000001` | no prediction |
| `USB_VBUS_EN0` | `0x00000101` | no prediction |
| `GPIO_A_CNF`, `OE`, `OUT` | `0x20`, `0x20`, `0x00` | A5 `OUT` 0 as predicted; `CNF` 1 and `OE` 1 means A5 is an output driven low |
| `GPIO_CC_CNF`, `OE` | `0x18`, `0x18` | CC4 `CNF` 1 and `OE` 1; `OUT` 0 as predicted (owner's report) |
| `GPIO_S_IN` | `0x00000000` | one sample, not an edge count |
| S7 `CNF` 1, `OE` 0, V4 `CNF` 0 | from the TACH line | as predicted |
| `GPIO_V_CNF`, `OE` | `0x06`, `0x07` | V4 bits are 0, so `CNF` 0 as predicted |
| `PMC_NO_IOPOWER` | `0x00000000` | no prediction |

`PMC_PWR_DET_VAL` is on the photo as `005C3C2C` with low confidence in the digits, and its summary line is garbled by finding 3; it is not used here.

## What this does not show

No fan rotation, tach edge count, 5 V voltage or temperature response was measured. A single run says nothing about repeatability. The state is the one after hekate and U-Boot on this launch path, with Auto Boot off.

## Fixes made offline

Code changed after this analysis, checked offline only. T3-R is not run again, and the fixed image has no Mac check yet.
1. The derived values (`TMRUS` advance, PWM gate, PWM class) are computed in `step_read` before the RESULT line is drawn; the end-of-loop assignment is gone. This also fixes the CLK and PWM result lines, whose tags came from fields that were still unset at draw time.
2. Summary lines have their own rows: `SWITCH_T3R_SUMMARY_FIRST_ROW` (screen row 20) for the new CAR line, then FAN, ROUTE, RAIL, TACH and COMPLETE on rows 21 to 25, full width, below the two read columns and above the notes at row 26. A full-width line fits the screen (`LEFT + 104 * 12 <= 1280`, asserted in `diag.c`).
3. A CAR line shows both raw words, bit 17 of each and whether the gate is open. A comma in two message strings (the font has none) is gone, and a test now checks every screen string against the font.
4. Tests: the host test keeps a copy of the result structure as each step is drawn and checks the timer line with the raw words of this run (`0x0680CFDD`, `0x06C1B6A5`, `ADV 4253384 US`), the CLK and PWM tags, the CAR line, that no summary row equals a read row, that read cells do not collide, and that every string uses glyphs the font has. Changing the code back to the old order or the old rows makes these fail (checked with two mutations on the host).
5. The Linux flat hash of `diag_t3r` is now `075d6816...`; the ten earlier hashes are unchanged and `diag_images` reports eleven inspections.
