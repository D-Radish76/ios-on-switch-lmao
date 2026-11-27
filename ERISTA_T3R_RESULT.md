# T3-R: result of the first run

Run by the owner on 2026-11-27 (report 16:10 JST) with the image of commit `4a903d590`, one run, one photograph. Analysis only. T3-R is not run again, no fan or 5 V code is written, `boot` stays 0, the feature mask stays 0, T3 and T4 are not waived, the first XNU run is on HOLD.

Values below were read from the photograph and the owner's report. The photo is a rotated phone shot; digits I could not read twice are left out. Nothing was measured by this analysis.

## Three separate findings

| | Finding | Class |
| --- | --- | --- |
| 1 | The 22 reads were made, the loader stopped with code 0000 and hekate came back by itself | success |
| 2 | The last `TMRUS` line says `DID NOT ADVANCE` although the counter advanced | display bug in T3-R, not a timer fault |
| 3 | The summary lines overwrite the result lines of reads 2 to 6 and spill into the right column | screen layout bug in T3-R |
| 4 | `PWM_CSR_1` is `0x00000000`, not `0x81000000` | a result that differs from the prediction, cause open |

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

What the source says (hekate v6.5.5 `fan.c`, read through a summarising fetch, not byte-checked): `fan_set_duty` writes `PWM_CSR_1` in its one-time initialisation and in every duty change; the off value is the enable bit with `0x100 << 16`. If `fan_set_duty` had run on this path and nothing reset the PWM block afterwards, the word would be `0x81000000`. It is not. So at least one of these holds, and this run cannot tell which:

| Candidate | What it needs | What would settle it |
| --- | --- | --- |
| `fan_set_duty` never ran on the Nyx, hekate and U-Boot path before our loader | no caller on that path | reading the callers of `fan_set_duty` in hekate and Nyx v6.5.5 for the launch path |
| The PWM block was reset after it ran | a reset pulse of `RST_DEVICES_L` bit 17 with the clock left on | the `hw_deinit` and clock code of hekate and U-Boot; the raw CAR words (record word) do not show a past pulse |
| The word was never anything but its reset value | the reset value of `PWM_CSR_1` is zero | the TRM, which is not in the repository |

The pinmux words do not choose between them. `LCD_GPIO2` `0x74` (input, parked, tristate, pull down) is what hekate's off branch writes, and `CAM1_PWDN` `0x59` is what its init writes on T210; both could equally be reset defaults. That is unknown here: the reset values are in the TRM.

What it means for the fan work, and what it does not:
- The handoff state is not hekate's off word. A later fan step must not assume `0x81000000`; it has to program the whole word itself and read it back.
- `LCD_GPIO2` is tristated, parked and pulled down, so the pad is not driving the fan line. That is an inference from the pad word. No fan speed, tach edge or 5 V rail voltage was measured, so "the fan is off" is not established.
- The one off-state word T3-R was built to confirm (`OFF AS HEKATE`) was not seen. The prediction table (`ERISTA_T3R_DESIGN.md`) is wrong for this row.

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

## Fixes, not made

For a later round, only after an owner decision, and a changed image has a new hash and needs its own Mac check and approval. T3-R is not run again now.
1. Compute the `TMRUS` advance in `step_read` before the last RESULT line is drawn, or have `result_line` compute it from the two values it already holds.
2. Give the summary lines their own rows (21 to 25), not `place()`, and keep each under 50 cells or give it the full width on rows no read uses.
3. Tests that look at the structure given to the screen callback at draw time, and a check that no summary row equals a read row.
4. Show the two CAR words on screen so the gate decision is auditable from a photograph.
