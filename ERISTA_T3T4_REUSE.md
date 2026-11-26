# T3 and T4: what hekate and Switchroot already do

Offline review of 2026-11-27 of whether existing code can replace new fan and watchdog code in the loader. Nothing here is implemented, approved or waived. The MMIO allowlist is unchanged; T3 and T4 stay on HOLD and
the waiver is undecided. The hekate sources are the `v6.5.5` tag, the version the console runs, read through a fetch tool that answers from the raw files and may summarise, not from a local checkout, so quotes are close but not byte-checked. They agree with the earlier read of `master`.
Files at the tag: [`bdk/soc/hw_init.c`](https://github.com/CTCaer/hekate/blob/v6.5.5/bdk/soc/hw_init.c), [`bdk/thermal/fan.c`](https://github.com/CTCaer/hekate/blob/v6.5.5/bdk/thermal/fan.c),
[`bdk/soc/timer.c`](https://github.com/CTCaer/hekate/blob/v6.5.5/bdk/soc/timer.c), [`bootloader/main.c`](https://github.com/CTCaer/hekate/blob/v6.5.5/bootloader/main.c), [`bootloader/l4t/l4t.c`](https://github.com/CTCaer/hekate/blob/v6.5.5/bootloader/l4t/l4t.c), [`Makefile`](https://github.com/CTCaer/hekate/blob/v6.5.5/Makefile) and [`nyx/Makefile`](https://github.com/CTCaer/hekate/blob/v6.5.5/nyx/Makefile).
Not read: `soc/timer.h` (the values of `TIMER_FIQENABL_EN` and any reset-enable bits).

## Can the fan be kept on from hekate to XNU?

| Finding | Source | Certainty |
| --- | --- | --- |
| `hw_deinit` runs `vic_end`, `tmp451_end`, `fan_set_duty(0)`, `touch_power_off`, `jc_deinit` and `regulator_5v_disable(REGULATOR_5V_ALL)` only under `BDK_HW_EXTRA_DEINIT` | `hw_init.c` at v6.5.5 | read |
| Nyx's Makefile defines `BDK_HW_EXTRA_DEINIT`; the bootloader's Makefile does not | the two Makefiles at v6.5.5; the bootloader's defines are `BDK_MALLOC_NO_DEFRAG`, `BDK_EMUMMC_ENABLE`, `BDK_WATCHDOG_FIQ_ENABLE` and `BDK_RESTART_BL_ON_WDT` | read |
| The L4T path (`launch_l4t`) calls `hw_deinit` and `sd_end` and nothing about fan, 5 V, TMP451 or thermal; its PMC writes are scratch, sticky and secure-disable bits | `l4t.c` at v6.5.5: after `hw_deinit` and `_l4t_late_hw_config` it jumps to `BPMPFW_ENTRYPOINT`; the only regulator call is `max7762x_regulator_set_voltage(REGULATOR_SD1, ...)` | read |
| So the fan and the 5 V fan rail are off when Nyx hands over, and no hekate option or code on this path turns them on again | | follows from the above; it matches `ERISTA_THERMAL.md` (Nyx's launch ran `fan_set_duty(0)`). Which of Nyx and the bootloader runs `hw_deinit` last on a Nyx launch is still not traced |
| Switchroot Linux turns the fan on from its own `pwm-fan` driver, after the kernel is running; the Erista DTB's profile keeps it at 0 below 40 C estimated skin temperature | `ERISTA_THERMAL.md`, packaged DTB | read earlier |

So a fan that is on during the first XNU run has to be started by code that runs after hekate and before XNU. Nothing in hekate does that, and stock hekate is what the console runs. The run therefore starts with the fan off, as every Switchroot
boot does until its kernel's PWM driver loads. That is a precedent for the situation, not a measure of how long it is safe: how long Switchroot's fan-off window is on an Erista was not measured here.

## PWM clock, PWM and the 5 V rail

- Measured at the loader (T1 v3, T1-T and T2, one console, one reading each): `CLK_OUT_ENB_L` `0x9802D131` has bit 17 (PWM) set, `RST_DEVICES_L` `0x049102CA` has bit 17 clear. The PWM clock is on and the block is out of reset when the loader runs. This matters because hekate's fan code enables the clock only on an Aula, so on an Erista it relies on the display's backlight PWM having left it on. It is a reading at that time, not a guarantee later.
- Not measured: `PWM_CSR_1`, the 5 V fan rail (GPIO A5 and CC4, `SATA_LED_ACTIVE`, `USB_VBUS_EN0`, the PMC `NO_IOPOWER` and `PWR_DET_VAL` bits), the pinmux of `LCD_GPIO2` and `CAM1_PWDN`. Reading them is a new MMIO read; it is not approved.
- By hekate's code the state after Nyx's deinit is: duty written as off (`PWM_CSR_1` = EN | `0x100 << 16`), the 5 V fan rail disabled, `LCD_GPIO2` parked with pull-down. This is read from the source, not seen on the console.

## What of hekate's code can be reused

- hekate is GPL-2.0 and so is this repository (`LICENSE`), so its `fan_set_duty` sequence can be ported with its notices. The files carry their own copyright lines, and the port must keep them.
- The reusable part is the sequence, not a library: the loader is freestanding and has its own register helpers. `fan_set_duty` also initialises on first call (tachometer pinmux with a pull-up on a T210, `PWM_CSR_1` to the off value, `LCD_GPIO2` to PWM1, GPIO V4 to SPIO), and its on path calls `regulator_5v_enable(REGULATOR_5V_FAN)`, which reaches the PMC and the USB VBUS pins on a T210. That is the T3 design's register list; reuse saves writing the order, not the writes.
- hekate's rpm count is a 2 s polled GPIO loop (`irq_count / 2`, then `* 30`). It is the only evidence-producing part of T3 and the part with the least slack: a fast fan loses edges.
- Nothing there reduces the new MMIO surface: pinmux, GPIO, PMC and PWM blocks stay as new allowlist entries. Reuse lowers the chance of a wrong sequence; it does not remove the writes or the need for a run.

## Watchdog: what hekate leaves

- `bootloader/main.c` calls `watchdog_start(5000000 / 2, TIMER_FIQENABL_EN); // 5 seconds.` before its LP0 and Minerva work and `watchdog_end()` after `minerva_init`. `watchdog_start` programs WDT4 with timer 9 (`TIMER_SRC(9) | TIMER_PER(1) | mode`); `watchdog_end` clears the timer, re-arms to clear interrupts, disables the counter and clears the interrupt. On the normal path no hekate watchdog is running when an entry is launched. If `watchdog_fired()` was true at start, the code jumps over both calls to `skip_lp0_minerva_config`.
- The mode word passed is `TIMER_FIQENABL_EN`; `timer.c` itself has no `SYSRESET` or `PMCRESET` bit. The header that defines the bits was not read, so "FIQ only, not a system reset" rests on the mode name and the absence of any reset bit in the code that is called. It is not a system reset and does not act on a hung A57. So hekate has no code that resets the console from a hung CPU complex, and its watchdog is not reusable for T4's purpose.
- At v6.5.5 `hw_init` writes `MAX77620_REG_ONOFFCNFG1`, `CNFGBBC`, the FPS registers, GPIO configuration and `SD2_CFG`, and sets up the low-battery monitor. The list shows no write to the PMIC watchdog registers. The PMIC watchdog is then in the state the part boots in, a default that was not read from a datasheet here and is not assumed.
- After a reset through `SYSTEM_RESET` (what the VM-done path issues) hekate is the next stage. Whether a PMIC watchdog enabled earlier survives that restart is the open question of `ERISTA_T3T4_MINIMAL.md`; nothing read answers it.
- `hw_init` clears `PMC_SCRATCH190` bit 0 (`WDT_DURING_BR`), a boot-ROM watchdog flag. What the bit controls here was not read.

## SOC_THERM and hang protection, evaluated separately

No part of the run needs either to work. The VM-done stop and the timed reset do not read a temperature (`ERISTA_FIRST_XNU_RUN.md`). Each is judged on its own below.

| | Independent hang protection | SOC_THERM thermtrip |
| --- | --- | --- |
| Acts on | a hang of any cause, after its period | temperature, whatever the cause |
| Existing code | none that resets a hung CPU complex: hekate's WDT4 is the BPMP's, started and ended inside `ipl_main`, with no reset bit in the code read | none in hekate at the files read; Switchroot's kernel drivers do it after Linux is running |
| What it needs | a reset-capable timer; the PMIC watchdog is the only candidate with a source, so an I2C5 driver and a disarm path | clocked and calibrated sensors, the thresholds, and a PMC command that makes a trip act |
| What can go wrong | an armed watchdog that is not disarmed resets a good run; one that is never armed protects nothing; whether it survives the VM reset is unknown | a trip level from uncalibrated sensors is wrong either way; it cannot be tested by heating the console |
| Does it cover a stuck fan? | only by ending the run after its period, a time limit that is not a temperature guarantee | yes, if calibrated, and nothing reviewed shows it is |
| New MMIO | PMIC I2C5 writes beyond T2's one-shot | SOC_THERM and PMC registers |

Neither is a precondition of the other. A run with a watchdog and no thermtrip is bounded in time and has no temperature limit; a run with thermtrip alone has a temperature limit that depends on calibration nobody has verified. A time limit of either kind is not treated as a safety guarantee. Which of them the owner requires, if either, is the owner's decision, and both stay on HOLD.

## Result

Existing hekate and Switchroot code gives the fan a known sequence to port and shows the PWM clock is on at the loader, but it cannot keep the fan on across the hand-over, and it contains no watchdog that resets a hung CPU complex.
The least new code for T3 is a port of hekate's fan sequence; the least for T4 is a PMIC watchdog driver with no precedent in hekate. Both are new MMIO and both stay on HOLD.
