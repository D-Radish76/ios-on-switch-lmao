# Erista temperature and fan: what the sources say

Research only. Nothing here is implemented, and no temperature, fan or SOC_THERM write is part of D0, D0-R or D1-pre.
Sources: hekate `8e1392d` (2026-11-11) `bdk/thermal/{fan,tmp451}.c`, `bdk/power/regulator_5v.c`, `bdk/soc/i2c.h`; Linux
`drivers/thermal/tegra/{soctherm,tegra210-soctherm}.c` and `drivers/irqchip/irq-gic.c` (master, sparse clone); the control
FDT inside the pinned `bl33.bin`; this repository's `SwitchThermal.cpp`, `SwitchSocTherm.cpp`.

## I2C and PWM as U-Boot sees them

| Part | Controller | In the pinned U-Boot's control FDT |
| --- | --- | --- |
| TMP451, address `0x4C` | I2C1, `0x7000C000` (hekate `I2C_1`) | `i2c@7000c000` has `status = "disabled"` |
| PMIC (MAX77620, MAX77621) | I2C5, `0x7000D000` (hekate `I2C_5`) | `okay`, and the only alias: `i2c5` |
| Fan PWM1 | `pwm@7000a000` | `disabled` |

So the pinned U-Boot has one I2C bus, the PMIC's, and no `pwm` device. `i2c bus` lists that bus alone; the TMP451 cannot be
read with `i2c md` as the pinned image stands. An earlier line in `ERISTA_LOADER_DIAG.md` took the `i2c5` alias for I2C1;
it is I2C5, and that line is corrected. The same FDT puts `mmc0` at `sdhci@700b0600` (SDMMC4, the internal storage) and
`mmc1` at `sdhci@700b0000` (SDMMC1, the SD card), which is why `mmc info` is never run before `mmc dev 1`
(`boot/diag/sd/README.md`).

Any read of the TMP451 needs an I2C transaction, which means the controller's clock, reset and FIFO registers are written.
It is not a read only step by any route: enabling the node in U-Boot's FDT (a patched image), or a loader that drives the
controller itself. The temperature plan's T1 is therefore a peripheral write step with a stale answer, and stays unimplemented
until reviewed as one. A value read after Nyx's `hw_deinit` is the temperature at that moment: the TMP451 is in shutdown
(config `0xC0`, `tmp451_end`) and keeps its last conversion. It proves the bus works. It says nothing about the present.

## Where hekate differs between Erista (T210) and Mariko (T210B01)

| Where | Erista | Mariko | This repository's driver |
| --- | --- | --- | --- |
| `fan_set_duty`, tachometer pin (CAM1_PWDN, GPIO S7) | pull-up | none | not set |
| `tmp451_init`, remote offset | 0 degrees C | -12.5 degrees C | -12.5 (`0xF3/0x80`), the Mariko value |
| `regulator_5v_enable`, with the fan | also `USB_VBUS_EN0` pinmux and GPIO CC4 driven low; `regulator_5v_usb_src_enable` drives CC4 high for a USB source | battery regulator (GPIO A5) only | GPIO A5 only |
| PWM clock | fan code does not enable it; the display's backlight PWM does (only Aula's fan code enables it) | same | enables it |
| Duty | inverted, field `236 - duty`, capped at 236, stop is `0x100 << 16` | same | scale 256, field `256 - value` |

Consequences: the repository's fan, TMP451 offset and 5 V code is the Mariko path and must not run on the Erista as it is.
An Erista fan driver needs the CC4 handling and the tachometer pull-up read against the hardware, the clock source for PWM
settled (hekate's display driver enables it, and whether Nyx's `hw_deinit` leaves it enabled is not read), and the duty
scale agreed (hekate's cap of 236 against the repository's 256).

## SOC_THERM and the hardware trip

- `THERMTRIP_CTL` on T210 (Linux `tegra210-soctherm.c`): bit 31 any, 30 memory, 29 GPU, 28 CPU, 27 TSENSE (the PLL
  sensor); nine bit thresholds at bit 0 (TSENSE), 9 (CPU), 18 (GPU and memory). `SwitchSocThermWindow.h` uses the same layout.
- Linux programs the trip in software (`thermtrip_program` sets threshold and enable bits). Nothing read says the hardware
  arms one at reset. hekate's bootloader build sets none, and BL31 was not read beyond its hash. At our handoff the hardware
  trip is therefore treated as absent, not as armed with a default.
- Reading a sensor needs its enable, clock and the fuse calibration (`soctherm-fuse.c`); whether the repository's bring-up
  (`SwitchSensors.cpp`, written for the OLED) calibrates correctly on a T210 is not shown.
- The repository's limits (PLLX trip 96 C, CPU and GPU 127 C, PMC reset, shutdown at a diode reading of 92.5 C) were chosen for
  the OLED. They are not reviewed for the Erista and are not a reference for it.

## What is left open

1. How to read the TMP451 at all: patched FDT or a controller driver in the loader, and what else sits on I2C1.
2. Whether the PMIC thermal shutdown is enabled and at what threshold (not read).
3. The CPU rate and voltage hekate leaves on the Erista (`SwitchThermal.cpp` has only the OLED's figures).
4. Whether the PWM clock is on after Nyx's `hw_deinit`.
5. Whether BL31 (the pinned binary) touches SOC_THERM.

## What to do until then

D0-R and D1-pre add no fan, no sensor and no peripheral write. Their durations are bounded by what they run (see the stage
deadlines in their checklists), a person is present, the console is cool at the start and is powered off between runs. A
run is not extended to wait for a result. No kernel runs on the Erista before items 1 to 3 are answered.
