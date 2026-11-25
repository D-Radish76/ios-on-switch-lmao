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

T1 to T4 are designed in `ERISTA_THERMAL_DESIGN.md`; T1 is implemented (`ERISTA_T1.md`).

## What is left open

1. How to read the TMP451 at all: patched FDT or a controller driver in the loader, and what else sits on I2C1.
2. Whether the PMIC thermal shutdown is enabled and at what threshold (not read).
3. The CPU rate and voltage hekate leaves on the Erista (`SwitchThermal.cpp` has only the OLED's figures).
4. Whether the PWM clock is on after Nyx's `hw_deinit`. Hekate's own launch paths differ in what they leave (`ERISTA_THERMAL_DESIGN.md`).
5. Whether BL31 (the pinned binary) touches SOC_THERM.

## What to do until then

D0-R and D1-pre add no fan, no sensor and no peripheral write. Their durations are bounded by what they run (see the stage
deadlines in their checklists), a person is present, the console is cool at the start and is powered off between runs. A
run is not extended to wait for a result. No kernel runs on the Erista before items 1 to 3 are answered.

## D1: who stops a run, and what is known to protect the SoC

Two different things, kept apart. A **stop rule** is a person acting on what the screen shows. A **protection** is something
that acts without a person. D1 has stop rules. No protection is verified on this console, so none is relied on.

| | Stop rule (a person) | Protection (hardware or firmware) |
| --- | --- | --- |
| D1 | The stage deadlines in `boot/diag/sd_loader/RUN_CHECKLIST.md`; hold the power button for at least 12 s (Nintendo's documented force-off; not tried against this firmware). | None verified. Candidates below, each unread on the Erista. |
| Ends the run when | the person sees a stall and holds the button | an unread thermtrip, PMIC or BPMP behaviour fires |

### Provisional deadlines, and what they add up to

Adopted as provisional: SD read 15 s, exporter `go` 10 s. They are budgets, not measurements: no run has been timed (the D0
and D0-R videos are not read). The table adds each stage's deadline, so it is the longest a run can take before a person
must have stopped it. The expected run is far shorter and ends in the fixed 15 s wait.

| Stage | Deadline | Running total |
| --- | --- | --- |
| Entry selected to the first U-Boot line | 20 s | 20 s |
| Six gaps between U-Boot's lines up to `READ SD SECTOR 0` (board, framebuffer, DTB, exporter loaded and checked, loader written) | 6 x 10 s | 80 s |
| `READ SD SECTOR 0` to the exporter's `## Starting application` (one block read, CRC32) | 15 s | 95 s |
| Exporter `go` | 10 s | 105 s |
| Return to `HANDOFF` | 10 s | 115 s |
| `HANDOFF` to the loader's first line | 10 s | 125 s |
| Loader report to `RESET IN 15 S` | 20 s | 145 s |
| Wait and reset to hekate | 25 s | 170 s |

A stall in a stage costs the running total up to the previous row, plus the deadline, plus the person's reaction (not
measured; five seconds is an assumption), plus the 12 s hold. A stall in the exporter `go` is about 95 + 10 + 5 + 12 =
122 s from the start, with the fan off throughout.

### What an abnormal stop looks like

| Case | What the core does | Thermal exposure until a person acts |
| --- | --- | --- |
| U-Boot takes an exception (abort, bad IRQ) | prints `Resetting CPU ...`, `panic`, then 100 ms later calls the same reset the script's `reset` uses (`0x3DA30` calls `0x2358`; a `b .` follows only if that returns). The script's `reset` has landed in hekate on every run so far. | Short. Not a stall. |
| `go` into the exporter or a helper never returns and raises no exception (a loop, a wait) | spins at whatever clock hekate left, display on, nothing resets it | Until the person holds the button. This is the case the stop rule is for. |
| The card does not answer | the driver times out and `read mmc 1` returns an error, so the script reaches `SD_REFERENCE_READ_FAILED` and the dwell and reset | Bounded by the dwell. Not read in the binary: the driver's timeout. |
| A loader check fails at EL2 | parks in `wfe` with all interrupts masked, nothing resets it | Low power, but until a person acts. Same as D0. |
| The loader's `wfe` wait never wakes (counter or event stream) | `wfe` forever | Low power, until a person acts. |

Nothing in D1 raises the CPU clock, the voltage or the EMC rate. D1 runs on whatever hekate left after Nyx's `hw_deinit`,
which has not been read.

### What was read about protections on the Erista

- **SOC_THERM thermtrip is configured by software.** The Linux binding says `nvidia,thermtrips` sets the temperature at
  which the hardware asserts a trigger to the PMIC, and the PMIC may reset or shut down; it does not say the trip is armed
  at reset ([binding](https://www.kernel.org/doc/Documentation/devicetree/bindings/thermal/nvidia%2Ctegra124-soctherm.txt)).
  NVIDIA's L4T guide states that Tegra X1 thermal management is done by software on the main CPU, with the hardware
  shutdown as the last fallback ([L4T power management](https://docs.nvidia.com/jetson/archives/l4t-archived/l4t-283/Tegra%20Linux%20Driver%20Package%20Development%20Guide/power_management_tx1.html)).
  An NVIDIA engineer describes the last step as a hardware shutdown by asserting the PMIC's reset pin
  ([forum](https://forums.developer.nvidia.com/t/jetson-nano-devkit-reboots-when-overheating/128264)). None of this says it
  is armed before a kernel programs it.
- **The packaged Erista DTB shows what a kernel programs.** `bazel/pinned/firmware/erista-sku0.dtb`: `nvidia,thermtrips`
  of CPU 102.5 C and GPU 103 C; CPU trips 89 C (passive), 98.5 C (hot), 102.5 C (critical); GPU 90.5 C, 100 C, 103 C; AO
  critical 110 C. These are NVIDIA's reference values for this board in a Linux DTB, not a hardware default and not what runs
  under U-Boot.
- **The fan is a kernel function too.** The same DTB's `pwm-fan` and `thermal-fan-est` profile `Console` keeps the fan at 0
  below 40 C estimated skin temperature, about 20 percent from 40 C, 60 percent from 53 C and full from 58 C. U-Boot has no
  fan device (`ERISTA_THERMAL.md`, "I2C and PWM as U-Boot sees them").
- **The MAX77620 has die temperature alarms at 120 C and 140 C**, fixed, reporting only above or below
  ([Linux binding](https://git.pengutronix.de/cgit/barebox/tree/dts/Bindings/thermal/max77620_thermal.txt)). The binding
  describes interrupts only, and it is the PMIC's die, not the SoC's. The packaged DTB also lists the battery charger's
  thermal regulation threshold as 120 C, which is moot with the charger out.
- **A static scan of the pinned `bl31.bin` and `bl33.bin` finds no `0x700E2000` (SOC_THERM) or `0x700E3000` constant and no
  `movk #0x700e`.** `bl33.bin` holds `0x7000E400` once, the PMC address in its control FDT. This shows no direct access in
  these images, not that none exists: an address computed from a table is not seen.

### What this changes

- The stage deadlines stay as the stop rule. The 12 s hold makes the worst case longer than the deadline, so the checklist now
  says to hold at once when a stall is suspected, not to wait for the deadline to pass a second time.
- A protection is not assumed. The hardware trip is treated as absent, as before.
- T1 to T4 stay independent work. The open items 1 to 5 above stay open; the scan adds that neither pinned image touches
  SOC_THERM directly, so item 5 reads: not seen in a static scan of BL31 and U-Boot.
- Before D1 runs on hardware the reviewer should say whether the 122 s worst case with the fan off is acceptable on this
  console at room temperature, or whether T3 (the fan) comes first.
