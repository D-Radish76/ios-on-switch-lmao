# Erista thermal management: T1 to T4

Design only. Nothing here is implemented, nothing was run on the console, and no register write below is approved.
Sources: hekate 8e1392d (`bdk/thermal/fan.c`, `tmp451.c`, `power/regulator_5v.c`, `soc/i2c.c`, `gpio.c`, `timer.c`,
`hw_init.c`, `clock.c`), the pinned `bl31.bin` and `bl33.bin`, and this repository's `SwitchSocTherm*`. Background and the
open items are in `ERISTA_THERMAL.md`. D1 stays as built.

## What hekate does on the Erista

| Part | Erista behaviour | Differs from Mariko |
| --- | --- | --- |
| Fan rail | `regulator_5v_enable`: pinmux `SATA_LED_ACTIVE` (0x4C) = 1, GPIO A5 output high (battery 5V regulator enable). Then on T210 only: pinmux `USB_VBUS_EN0` (0x1A8) = LPDR\|1, GPIO CC4 output low. Clears the PMC `NO_IOPOWER` GPIO bit and `PWR_DET_VAL` 3.3V GPIO bit | CC4 and USB_VBUS_EN0 are skipped on Mariko |
| Fan PWM | `PWM_CSR_1` (0x7000A010) = `EN` \| (level << 16), level = 236 - duty (inverted, duty capped at 236). Pinmux `LCD_GPIO2` (0x20C) = 1 routes PWM1. GPIO V4 to SPIO | PWM clock is enabled by `fan_set_duty` only on Aula; on the Erista the clock is assumed already on |
| Fan off | duty 0: CSR = `EN` \| (0x100 << 16), then `regulator_5v_disable` (A5 low, CC4 low), `LCD_GPIO2` = tristate, pull down | same call |
| Tachometer | pinmux `CAM1_PWDN` (0x1EC) = tristate \| input enable \| pull up (T210) \| 1; GPIO S7 input. `fan_get_speed` counts edges for 2 s: rpm = (edges / 2) x 30 | pull up is T210 only |
| Fan curve (Nyx) | temp (whole C) >= 52: 102, >= 47: 76, >= 42: 51, <= 39: 0; 40 and 41 keep the last duty | same |
| TMP451 | I2C index 0 (controller 1, base 0x7000C000), address 0x4C. Registers: PCB 0x00, SoC (remote) 0x01, config 0x09, rate 0x0A. `tmp451_init`: config 0x80 (alarms off, range 0 to 127), rate 9 (31 ms) plus one read, then rate 2 (4 s). `tmp451_end`: config 0xC0 (shutdown) | SoC offset registers 0 on Erista |
| Value format | register >> 4 x 0.0625 C; hekate computes `((v >> 4) * 625) / 100` | same |
| I2C driver | 100 kHz via `CLK_SOURCE_I2C1` (source 6, divider 4, 4.8 MHz); `CLK_DIVISOR` (5 << 16) \| 1; `BUS_CLEAR_CONFIG` with `CONFIG_LOAD`; send timeout 100 ms; NOACK is status bits 0xF | same, other buses differ |
| Watchdog | `watchdog_start(us, mode)` programs WDT4 (the BPMP's) with timer 9. Hekate's own call uses `FIQENABL` only, no reset bit. `SYSRESET_EN` (bit 14) and `PMCRESET_EN` (bit 15) exist in the definitions and are not used by any call | BPMP only |
| THERMTRIP | Only `APBDEV_PMC_DIRECT_THERMTRIP_CFG` (0x474) is defined. Nothing in hekate writes it or SOC_THERM | none |

Hekate never arms a hardware thermal reset. Its protection is the Nyx loop: read the TMP451 every few seconds and set the
fan from the table.

### State at our entry depends on how hekate was left

`hw_deinit` runs `tmp451_end`, `fan_set_duty(0)` and `regulator_5v_disable(ALL)` only when `BDK_HW_EXTRA_DEINIT` is
defined. Nyx's Makefile defines it, `bootloader/` does not.

| Launch path | Fan rail | TMP451 | Clock |
| --- | --- | --- | --- |
| Nyx launches the entry (`_launch_hos` then `hw_deinit(false)`) | off | shutdown (config 0xC0) | BPMP scaled to normal, DRAM 204 MHz |
| Plain bootloader menu (`l4t.c` `hw_deinit(sld_type)`) | whatever Nyx last set; may be on | whatever Nyx last set | same |

D0, D0-R and D1 runs so far were started from the menu the user uses. Which path that was is not recorded. T1 must record
it (see T1) rather than assume it.

## T1 to T4

Order is by risk. Each stage is its own review and its own run. A stage never starts the next one.

| | Purpose | Writes | Reads |
| --- | --- | --- | --- |
| T1 | Prove the I2C path reaches the TMP451 | I2C1 controller setup, pinmux and clock if not already on | TMP451 0x00, 0x01, 0x09, 0x0A |
| T2 | Make the sensor produce a live value | TMP451 config (clear shutdown), rate | TMP451 repeatedly |
| T3 | Turn the fan on and prove it turns | 5V rail GPIOs, PWM, pinmux | tachometer GPIO S7, TMP451 |
| T4 | Bound a longer run without a person | SOC_THERM trip and PMC reset, or a watchdog | the same sensors |

### T1: communication check

Operations, in order:

1. Read `CLK_OUT_ENB_L` and `RST_DEVICES_L` (CAR) for the I2C1 bit. Read `CLK_SOURCE_I2C1`. If the controller is already
   clocked and out of reset, change nothing in CAR.
2. If hekate left it clocked, read I2C1 `STATUS` (0x1C) and `CNFG` (0x00). If the controller is not clocked, T1 stops
   without writing: the missing clock is the answer.
3. Pinmux read of the I2C1 SCL and SDA pads (0xBC and 0xC0, `PINMUX_AUX_X_I2C_SCL(0)` and `_SDA(0)`). No write.
4. Controller setup as `i2c_init(0)`: `CLK_DIVISOR`, `BUS_CLEAR_CONFIG`, `CONFIG_LOAD`, poll `INT_STATUS` bit for bus clear done.
5. One write of the register pointer (0x01) then one read, then the same for 0x00, 0x09 and 0x0A. Each transfer is
   bounded by a 100 ms timeout.

Side effects: step 4 pulses SCL up to nine times if SDA is held (bus clear). The TMP451 sees register pointer writes only,
no config write. Failure behaviour: NOACK or timeout is recorded as a code and the stage ends; there is no retry loop and
no second transfer after a failure.

T1 result is a communication check. In shutdown mode (config bit 6) the TMP451 returns its last conversion or the reset
value, so a number from T1 is stale and is never a temperature. The record must carry: which launch path, config value, and
whether the value is the power-on default (0 for both channels).

### T2: live value

Operations: write config 0x09 = 0x80 (hekate's value: shutdown cleared, alarms masked, range 0 to 127), write rate 0x0A =
9, wait at least two conversion times (62 ms), read 0x01 again, then rate 2.

Side effects: the TMP451 starts converting and draws more current. Alarms are masked so the ALERT pin cannot interrupt
anything. One-shot is the alternative: with shutdown set, a write to the one-shot register (0x0F) triggers a single
conversion. One-shot leaves the sensor in the state hekate expects after `tmp451_end`, so it is the lower-impact first run.

Failure behaviour: if the value after the wait equals the T1 value and the config readback is not 0x80 (or 0xC0 for
one-shot), the write did not take; the stage ends and reports the readback. Nothing else is changed.

### T3: fan

T3 is the first stage with side effects outside the sensor. Order matters because the rail is switched before the fan is
driven.

1. Read GPIO A5 (OE, CNF, OUT) and CC4, and the PWM CSR_1. Record them. If the PWM clock bit is not set in CAR, stop: the
   fan code on Erista assumes the clock is on, and enabling it is a CAR write that this design does not take on.
2. Tachometer first, no rail: pinmux `CAM1_PWDN`, GPIO S7 input. Read for 1 s with the fan unpowered: edges must be zero
   (otherwise the pin is not the tach or it floats).
3. Rail: `SATA_LED_ACTIVE` = 1, A5 high, `USB_VBUS_EN0` = LPDR\|1, CC4 low. Wait for the rail (hekate has no delay in
   `regulator_5v.c` or `fan.c`; 10 ms is this design's choice).
4. PWM at the lowest duty the curve uses (51). `PWM_CSR_1` = `EN` \| ((236 - 51) << 16), `LCD_GPIO2` = 1, V4 to SPIO.
5. Count S7 edges for 2 s as `fan_get_speed` does. Pass: rpm above a floor taken from the first measured value, and edges
   within a band of the readback of CSR_1.
6. Raise to duty 102 and repeat; the count must rise.
7. End state is chosen by the stage: fan off through the hekate sequence (duty 0, rail off, pin parked), or fan on at a
   fixed duty for the next stage.

Failure behaviour: no edges at duty 51 means the fan is not running. The stage turns the fan off by the off sequence
above, reads back A5, CC4 and CSR_1, and reports. It does not raise the duty to see if more helps.

### T4: bound without a person

T4 is the only stage that can end a run by itself. Two candidates, to be chosen by the reviewer:

- **SOC_THERM trip with PMC reset.** This repository's `SwitchSocTherm.cpp` arms `THERMTRIP_CTL` (SOC_THERM + 0x80) and sets
  `PMC_SENSOR_CTRL` (PMC + 0x1B0) bit 1. Those values are for this repository's kernel. The register layout and the PMC bit
  are not checked against an Erista source in this tree. SOC_THERM (0x700E2000) is not mapped by the pinned U-Boot, so this
  needs a loader-side mapping.
- **A watchdog.** WDT4 with timer 9 is the BPMP's and is what hekate uses, with the FIQ only. A reset needs `SYSRESET_EN` or
  `PMCRESET_EN` in the config. The BPMP is not running anything here, and which watchdog resets the system when only an A57
  runs is not read. A CPU-side WDT (WDT0 to WDT3) is the likelier fit and is not documented in any file read here.

T4 does not exist until its register map is sourced for the Erista. T3's pass is not a protection for T4.

## Stop conditions

The rule: any condition below ends the stage and puts the console in a known state. No condition is cleared by a retry.

| Condition | Detected by | Stage ends with |
| --- | --- | --- |
| Sensor does not answer | NOACK or 100 ms timeout on T1's first transfer | no further I2C; fan state unchanged |
| Sensor answers, value does not move | T2 value equal across two reads 4 s apart in rate 2 with the fan off and the SoC loaded | report `STALE`; no safety use |
| Value out of range | below 0 or above 127 (range 0 to 127 selected), or the 0x7F, 0x80 saturation | report; the stage ends |
| Temperature at or above the stop line | live value (T2 passed) | fan on if T3 passed, else power off by the reset path |
| Fan does not spin | no edges at duty 51 in 2 s (T3 step 5) | fan off sequence, report |
| Fan spins and the sensor is dead | T3 passed, T2 failed | the run does not proceed; no temperature claim |
| Edge count falls during a run | rpm under half the T3 value | treated as a fan fault |
| Any readback differs from what was written | read after write | stage ends |

The stop line is a decision for the reviewer. For orientation: hekate raises the fan at 42 C and leaves it off at 39 C or
below; the packaged DTB's CPU critical trip is 102.5 C. The TMP451 reads the SoC's remote diode, which is not the same
point as a CPU or GPU group sensor.

## Verifying the fan and the reading

**Fan turns.** Edge count on GPIO S7 (as `fan_get_speed`), with a rail-off baseline of zero, at two duties. Both rise. The
absolute figure is unknown until measured; the count is compared to itself.

**The reading is current.** Three checks, none enough alone:

1. Config readback is 0x80 (or 0xC0 in one-shot) after the write.
2. Value changes over time in rate 2 on a loaded SoC; a frozen value over a minute is a failure.
3. The two TMP451 channels differ plausibly: PCB (0x00) and SoC (0x01). Both read as zero means the power-on default.

A cross-check against SOC_THERM or the PMIC die alarm is possible only after T4's register work and is not assumed.

## Protection independent of a person

| Mechanism | Armed by default | State on the Erista |
| --- | --- | --- |
| SOC_THERM thermtrip to PMIC | No: software programs the thresholds (Linux binding) | Unknown at our entry; not read. No SOC_THERM constant found in the pinned images |
| `PMC_SENSOR_CTRL` reset enable | No | Unknown |
| BPMP WDT4 | No: software starts it | Hekate starts it for 5 s around DRAM training only, then ends it |
| MAX77620 die alarms (120 C, 140 C) | Interrupt only | Does not reset by itself |
| Nintendo force-off | Person | 12 s hold |

None can be relied on. A check that reads `THERMTRIP_CTL` and `SENSOR_CTRL` would settle "armed at entry or not" and is
read-only, but SOC_THERM is not mapped in U-Boot, so it needs the loader at EL1.

## D1 deadlines

D1 as built has a worst case of about 122 s from the start (stall in the exporter `go`) and 170 s in total. Can it be
shorter? The binary does not change, so only a successor can.

- The 20 s first-line budget and the six 10 s gaps (80 s of the 170) come from not having measured the D0 and D0-R
  durations. With measured times, 5 s gaps would cut the total to 140 s. That needs the videos read, not a guess.
- The final wait of 15 s and the report-to-reset 20 s are inside the loader and the script. They shorten only in a new
  build.
- A stall's cost is mostly the reaction plus the 12 s hold. A rule to hold the button at the first sign of a stall,
  already in the checklist, is the biggest lever and costs nothing.

None of these makes the run protected. They reduce the exposure, which stays unmeasured.

## Cache audit: second review

Order and barriers in the D1 script, read from the pinned binary (`ERISTA_CACHE.md`):

- D-cache clean (`dc civac` per line, then `dsb sy`) inside `loados`. The `dsb` completes before `bootm loados` returns.
- `0xAA001920`: `ic ialluis; isb; ret`. `ic ialluis` invalidates the inner-shareable I-cache; `isb` synchronises the fetch
  stream. Without a `dsb` between the invalidate and the `isb`, completion of the invalidate is not guaranteed before the
  `isb`, which the ARM ARM requires for the maintenance sequence.
- `0xAA001A64`: `dsb sy; isb; ret`, run right after. This is the `dsb` the first helper lacks. So the effective sequence is
  `ic ialluis; isb; dsb sy; isb`. The `isb` before the `dsb` is harmless; the `dsb` completes the invalidate, and the last
  `isb` makes the fetch stream see it. The order is adequate.
- Nothing writes to the exporter area between those helpers and `go`.

RAM-derived pointers and indirect MMIO in the loader: the device-tree pointer, the record and reference fields come from
RAM written by U-Boot and the exporter. `switch_diag_read32` has call sites only in `switch_diag_after_transition`, and its
addresses are constants from the board table, not from RAM. A RAM value is used as an address only after the loader's range
checks (the existing audit, `ERISTA_D1_DESIGN.md`). The audit cannot show that the exporter's own pointers are safe; the
exporter is a separate pinned image.

## Open for the reviewer

1. Is T1 as a stale-value check acceptable, or should T1 and T2 merge?
2. T2 one-shot or continuous?
3. T3: is the PWM clock assumption (already on) to be checked in T1 by a CAR read only?
4. T4: SOC_THERM trip or a watchdog, and who sources the Erista register map?
5. The stop line in degrees C.
