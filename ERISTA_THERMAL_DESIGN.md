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

### State at our entry

The user starts every diagnostic from Nyx, "More configs". Following hekate's code from there:

1. Nyx `_launch_hos` calls `hw_deinit(false)` with `BDK_HW_EXTRA_DEINIT` defined: `tmp451_end` (config 0xC0, shutdown), `fan_set_duty(0)`, `regulator_5v_disable(ALL)`. Then it jumps to hekate's `main`.
2. Hekate's `main` runs `hw_init`: `pinmux_config_i2c(I2C_1)`, `clock_enable_i2c(I2C_1)`, `i2c_init(I2C_1)` (`CLK_DIVISOR` 0x50001, bus clear once). Nothing in `bootloader/main.c` touches the fan or the TMP451.
3. The entry is an `l4t=1` entry, so `l4t.c` calls `hw_deinit(sld_type)` without the extra deinit. I2C1 stays clocked and configured.

So the expected state at the loader is: I2C1 clocked, out of reset, divisor 0x50001; fan rail off; TMP451 in shutdown with config 0xC0, which holds its last conversion. T1 reads these and does not assume them. A launch from the plain menu skips step 1 and can leave the fan and sensor as Nyx set them.

## T1 to T4

Order is by risk. Each stage is its own review and its own run. A stage never starts the next one.

| | Purpose | Writes | Reads |
| --- | --- | --- | --- |
| T1 | Prove the I2C path reaches the TMP451 | I2C1 transfer registers and the TMP451 register pointer, nothing else | CAR (I2C1 and PWM clock and reset bits), I2C1, TMP451 0xFE, 0x03, 0x04, 0x00, 0x01 |
| T2 | One conversion, read as a temperature | TMP451 one-shot command | TMP451 status, both temperature bytes |
| T3 | Fan on, shown to turn | 5V rail GPIOs, PWM, pinmux | tachometer GPIO S7, TMP451 |
| T4 | Hang and overheat protection | MAX77620 watchdog, SOC_THERM trip, PMC | the same sensors |

### T1: communication check (implemented, not run)

Separate from T2. Implementation and run notes are in `ERISTA_T1.md`.

- **Clocks first.** Read `RST_DEVICES_L`, `CLK_OUT_ENB_L` (I2C1 is bit 12, PWM bit 17), `CLK_SOURCE_I2C1` and `CLK_SOURCE_PWM`. A register of a gated block can hang the bus, so no I2C1 register is read or written unless the I2C1 clock is on and the block is out of reset. If it is not, T1 stops. It writes nothing to CAR. The PWM bits are read for T3 and no PWM register is touched.
- **No bus clear.** Linux's `i2c-tegra.c` runs bus clear only as recovery after an arbitration loss, not at init, and the pinned U-Boot never needs it for a healthy bus. hekate's `i2c_init` does it once at start, which `hw_init` has already done on the way here. T1 does not write `BUS_CLEAR_CONFIG`. If `STATUS.BUSY` is set before the first transfer it stops.
- **No controller setup.** T1 does not write `CLK_DIVISOR`. It requires hekate's value (0x50001) and stops on any other.
- **Transfers.** Normal mode exactly as `_i2c_send_normal` and `_i2c_recv_normal` do it: `CMD_ADDR0`, `CMD_DATA1`, `CNFG`, `CONFIG_LOAD` (0x25), `CNFG` with go, wait for `BUSY` to clear, check the NOACK bits. Ten transfers: five register pointer writes (one byte each, so the sensor's configuration is not written) and five one-byte reads. Each transfer happens once. A failure ends the stage.
- **Timeouts.** 100 ms for `BUSY` (hekate's bound), 1 ms for `CONFIG_LOAD`. Counter based.
- **Registers read from the TMP451.** Manufacturer ID 0xFE (TI documents 0x55), configuration 0x03, conversion rate 0x04 (their read addresses; 0x09 and 0x0A are the write addresses), local 0x00, remote 0x01. Status 0x02 is left for T2 because a read clears latched flags.
- **Use of the values.** Shown as hex bytes and never as a temperature. Not used for any decision. In shutdown mode they are the last conversion or zero.

The pointer write is not a sensor setting. A TMP451 read needs the pointer set first, and the datasheet gives no way to read without it.

### T2: one conversion

The current design is `ERISTA_T2_DESIGN.md` (2026-11-26), written after the T1 v3 run. The text below is the earlier draft.

One-shot is the first choice (TI TMP451 datasheet):

- Preconditions: config readback has SD (bit 6) set. If it does not, T2 stops without writing; clearing shutdown is a different, later decision.
- Command: a write to pointer 0x0F starts one conversion on both channels, then the part returns to shutdown. The datasheet says "write any value", and which transaction form it wants (send byte or write byte) is to be confirmed against its timing diagrams before code.
- Conversion time: 31 ms minimum, 34 ms typical. **No maximum is published.** The bound is a design choice: 100 ms, about three times typical, then stop. It is not a TI figure.
- Completion: poll status 0x02, BUSY (bit 7). BUSY is described for the ADC and the datasheet does not tie it to one-shot explicitly, so the first run records the BUSY sequence and does not treat a missed BUSY as a failure by itself.
- Remote diode fault: status OPEN (bit 2) set means the remote junction is open and the remote reading is not valid. The datasheet does not give the value read in that case, so OPEN decides, not the number. A short reads -64 C per the datasheet, outside the standard range (config bit 2 clear, 0 to 127 C).
- Read order: high byte 0x01, then low byte 0x10, so both belong to one conversion.
- An unchanged value is not a failure. Validity comes from: BUSY seen or conversion time elapsed, OPEN clear, config still SD, a value inside the range.
- Side effects: one write beyond T1's list. Status reads clear latched flags (RHIGH, RLOW, OPEN once the condition is gone), so T2 reads status once, records it, and does not poll more than needed.

### T3: fan

Source: hekate `fan.c`, `regulator_5v.c`, `gpio.c`, `pinmux.c`, `clock.c`. Initial state to read first, none written: CAR PWM clock and reset bits, `PWM_CSR_1` (0x7000A010), GPIO A5 and CC4 (CNF, OE, OUT), pinmux `SATA_LED_ACTIVE` (0x4C), `USB_VBUS_EN0` (0x1A8), `LCD_GPIO2` (0x20C), `CAM1_PWDN` (0x1EC). `PWM_CSR_1` is read only when the PWM clock is on.

Order, as hekate does it, with one change at the front:

1. Stop if the PWM clock is off or in reset. `fan_set_duty` enables the clock only on Mariko (Aula). On the Erista the backlight leaves it on and nothing in the fan code does. Enabling it is a CAR write and is not part of T3.
2. Tachometer with the rail off: pinmux `CAM1_PWDN` = tristate, input enable, pull up (T210), function 1; GPIO S7 input. Count edges for 2 s. Expect none.
3. 5V rail: `SATA_LED_ACTIVE` = 1, GPIO A5 output high; on T210 also `USB_VBUS_EN0` = LPDR | 1 and CC4 output low. `regulator_5v_enable` also clears the PMC `NO_IOPOWER` GPIO bit and `PWR_DET_VAL` 3.3 V bit, which are PMC writes.
4. PWM at duty 51, the lowest step of hekate's curve: `PWM_CSR_1` = EN | ((236 - 51) << 16), the signal is inverted. `LCD_GPIO2` = 1, GPIO V4 to SPIO.
5. Count S7 edges for 2 s.

RPM: hekate's formula is `(edges / 2) * 30` over 2 s, so two counted edges per revolution. Each edge is worth 15 rpm in that formula, which is the quantisation. The edges are counted by polling a GPIO, so a fast fan can lose edges and the figure reads low. No absolute rpm for this fan is known and none is assumed. The pass test compares counts: zero with the rail off, above zero at duty 51, and rising at the next step.

If no edge is seen at duty 51, T3 runs the off sequence and reports. It does not raise the duty and does not try again.

Safe state at a stop is hekate's off sequence: duty 0 (CSR = EN | (0x100 << 16)), rail off (A5 and CC4 low), `LCD_GPIO2` tristate with pull down. This restores the state hekate itself leaves after Nyx. Leaving the fan running is the alternative for a run that proved it turns. That choice is for the review: the off state is the one with a known hekate precedent.

### T4: hang and overheat protection

Goal for both: WDT against a hang, THERMTRIP against overheating. They protect against different things and neither replaces the other.

**Watchdog candidates**

| Candidate | Registers | Source | Open |
| --- | --- | --- | --- |
| MAX77620 watchdog, on the PMIC | I2C5 0x3C: `CNFGGLBL2` 0x01 (WDTEN bit 2, TWD bits 1:0: 0 = 2 s, 1 = 16 s, 2 = 64 s, 3 = 128 s, WDTSLPC bit 3, WDTOFFC bit 4), `CNFGGLBL3` 0x02 (WDTC, write 1 to clear), `ONOFFCNFG2` 0x42 bit 6 `WD_RST_WK` | Linux `max77620_wdt.c`, `max77620.h` | Expiry with `WD_RST_WK` set is a restart (Linux comment). Whether the PMIC keeps WDTEN across that restart is not shown anywhere read. If it does, hekate would be restarted every period unless something clears it: a boot loop. The Erista DTB lists `max77620@3c/watchdog` as disabled, so nothing in the packaged kernel uses it |
| Tegra TKE watchdog | The Erista DTB has `watchdog@60005100` (`nvidia,tegra-wdt-t21x`, also `0x60005088`, timer index 7, expiry count 4), disabled | the DTB | WDT4 (BPMP) is what hekate uses, with FIQ only. Which WDT resets the system from an A57 and how (`SYSRESET_EN`, `PMCRESET_EN`) is not in any source read; the TRM is needed |

**THERMTRIP**

- SOC_THERM at `0x700E2000` (DTB `reg`, also CAR `0x60006000` and CCROC `0x70040000`). `THERMCTL_THERMTRIP_CTL` at +0x80, fields from Linux `tegra210-soctherm.c`: bits 8:0 TSENSE (PLLX) threshold, 17:9 CPU, 26:18 GPU and MEM, bit 27 TSENSE enable, 28 CPU, 29 GPU, 30 MEM, 31 any. Thresholds are in 0.5 C steps (`THRESH_GRAIN` 500 mC). Linux sets the per-group enables and clears bit 31.
- The sensors must be running and calibrated first. Linux resets and clocks SOC_THERM (soctherm and tsensor clocks), then programs the sensors from fuses (`fuse_war` nodes, `sensor-params-*` in the DTB). The 2014 PMC patch says thermal reset "only works when the thermal sensors are calibrated". That is a driver's worth of work in the loader.
- What a trip does is not settled. In Linux, thermtrip on Tegra114/124 and later goes to the PMC, which performs an I2C write to the PMIC from `PMC_SCRATCH54/55` (offsets 0x258, 0x25C) when `PMC_SENSOR_CTRL` (0x1B0) has `ENABLE_RST` (bit 1) set. The DT node that gives that I2C command (`i2c-thermtrip`) is not in the Erista DTB. So the packaged DTB programs thresholds (`nvidia,thermtrips`: CPU 102.5 C, GPU 103 C) and supplies no PMC reset command. Whether the PMC command is a restart or a power off depends on the PMIC data, and it is not known here.
- Reset and power off are different outcomes. A restart returns to hekate, where Nyx's loop (`tmp451_init`, then `fan_set_from_temp`) resumes cooling if Nyx loads; the bootloader alone does not. A power off needs a human. Neither has been observed.

**What would be checked before arming either**

1. The reset destination restarts cooling: with the fan sequence from T3, a deliberate restart (the PSCI reset the loader already uses) returns to hekate and Nyx, where fan control is known from the source. This is checkable without heat.
2. A watchdog is disarmed on every exit path, including the failure ones, before the reset call, and its PMIC state after the restart is read.
3. THERMTRIP cannot be tested by heating the console. Its thresholds stay a documented value, set by review, and are not taken from the operating criteria below.

### Provisional operating criteria (candidates, not safety limits)

For a valid TMP451 remote (SoC) reading after T2 and T3:

| Reading | Action |
| --- | --- |
| below 40 C | candidate to start |
| 50 C or more | start no new work |
| 55 C or more | stop the diagnostic |

These come from the review. They are not an official limit and are not reused as THERMTRIP values.

## Stop conditions

Any condition below ends the stage and leaves the console in a known state. None is cleared by a retry or by raising a duty.

| Condition | Detected by | Stage ends with |
| --- | --- | --- |
| I2C1 clock off or block in reset | CAR read (T1) | no I2C1 register touched |
| Bus busy, or divisor not hekate's | I2C1 `STATUS`, `CLK_DIVISOR` read (T1) | nothing written, no bus clear |
| Sensor does not answer | NOACK, or `BUSY` still set after 100 ms (T1, T2) | no further transfer |
| Wrong manufacturer ID | 0xFE is not 0x55 (T1) | no further register read |
| Remote diode open | status OPEN set (T2) | reading marked invalid; no temperature use |
| No conversion result | BUSY never seen and the value is outside the range, or BUSY stays set past 100 ms (T2) | reading marked invalid |
| Fan does not turn | no edges at duty 51 in 2 s (T3) | off sequence, report, no retry, no higher duty |
| Edge count falls during a run | under half the T3 count | treated as a fan fault, off sequence |
| Reading at or above 55 C | valid T2 reading | diagnostic stops (provisional criterion) |
| Any readback differs from what was written | read after write | stage ends |

A temperature that has not changed is not on this list. It says nothing about a sensor that is working in a steady
console.

## Verifying the fan and the reading

**Fan turns.** Edge counts on GPIO S7 as in `fan_get_speed`: zero with the rail off, above zero at duty 51, higher at the
next step. The counts are compared with each other. No absolute rpm is known for this fan.

**The reading is current.** From T2: the one-shot write was accepted (config readback still SD), BUSY cleared or the 34 ms
typical time elapsed, OPEN clear, both temperature bytes read high first, and the value inside the selected range. A
cross-check against SOC_THERM or the PMIC is possible only after T4's register work and is not assumed.

## Protection independent of a person

| Mechanism | Armed by default | State on the Erista |
| --- | --- | --- |
| SOC_THERM thermtrip | No: software programs the thresholds and the sensors | Unknown at our entry; not read. No SOC_THERM constant in the pinned images |
| `PMC_SENSOR_CTRL` reset enable | No | Unknown. The Erista DTB has no PMC thermal reset command |
| BPMP WDT4 | No: software starts it | hekate starts it for 5 s around DRAM training, then ends it |
| MAX77620 watchdog | No: `WDTEN` is software | DTB marks it disabled |
| MAX77620 die alarms (120 C, 140 C) | Interrupt only | Does not reset by itself |
| Nintendo force-off | A person | 12 s hold |

None can be relied on. A read of `THERMTRIP_CTL` and `SENSOR_CTRL` would show whether the trip is armed at entry. SOC_THERM
is not mapped in U-Boot, so that read belongs in a loader stage after T1.

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

1. T1 is ready to review. The loader image, the allowlist and the checks are in `ERISTA_T1.md`.
2. T2: confirm the one-shot transaction form against the datasheet's timing diagrams, and accept 100 ms as the bound.
3. T3: the safe state at a stop, off sequence or leave running after a proven rotation.
4. T4: who supplies the TRM's watchdog and SOC_THERM description, and whether the MAX77620 watchdog is acceptable given
   that its state across a restart is unknown.
5. Whether D1 stays on hold until T3 and T4 exist, or until a smaller step.

## First XNU boot (N7-D)

Nothing in T1 to T4 protects a hung kernel yet: T3 and T4 are designs, and a release kernel arms no watchdog before userspace
(`ERISTA_STOP_PATHS.md`). The observation package (`ERISTA_FIRST_XNU_BOOT.md`) therefore relies on the person at the console: a time
limit, POWER held, Nyx's temperature before and after. T2's sequence is prepared on the host (`boot/diag/t2`) and its data sheet items are
settled except the conversion time's maximum and BUSY during a one-shot (`ERISTA_T2_DESIGN.md`). What a hang-safe mechanism needs is listed
in `ERISTA_STOP_PATHS.md`; which of them T4 supplies (MAX77620 watchdog, SOC_THERM trip, a TKE watchdog) is still the open question above.

