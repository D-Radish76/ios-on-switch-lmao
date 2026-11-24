# Erista research notes

Written without a console. "Confirmed" means read in a file at the cited place, or computed from one. "Inferred" and
"unknown" are labelled and are not decisions. Nothing here lifts `boot 0`, and no voltage or memory setting changes.

## 1. Display: InnoLux P062CCA-AZ1 (LCD ID 20 93 0F)

Confirmed
- The panel ID decodes to `PANEL_INL_P062CCA_AZ1` (0x0F20): hekate `bdk/display/di.c:472` computes
  `((raw >> 8) & 0xFF00) | (raw & 0xFF)`, so raw 0x0F9320 gives 0x0F00 | 0x20. The constant is `bdk/display/di.h:851`.
  This assumes the three ID bytes 20 93 0F are raw bytes 0, 1, 2 (low byte first).
- hekate hands the decoded ID to the OS through `PMC SECURE_SCRATCH113` (`bootloader/l4t/l4t.c:1146`). Our driver
  reads it back the same way (`TegraDisplayPower.cpp`, `tegra_display_panel_id`, `& 0xFFFF`).
- The T210 display path already exists: `drivers/NativeMobileDisplay/TegraDisplayErista.h` (tables copied from
  hekate's `di.c`/`di.inl`) and `TegraDisplayPower.cpp` (`erista()` from HIDREV, PLLD_MISC1, BTA timing, LCD pinmux,
  `initLcdPanel`). For this panel `initLcdPanel` sends EXIT_SLEEP_MODE, the 0xB9 unlock, the 0xB1 power control payload
  (`kPowerControlInl`), then SET_DISPLAY_ON.
- Suspend and resume refuse any panel but the OLED's: `NativeLegacyDisplay.cpp:5344` (`sc7Display`) panics otherwise.

Gaps found (backlight, confirmed by reading both sides)
- hekate lights an LCD panel with three steps (`bdk/display/di.c`): `display_backlight_pwm_init` (line 603) enables the
  PWM clock, writes PWM0's CSR enable bit and switches pin LCD_BL_PWM to function 1 (PWM0); `display_backlight(true)`
  (line 620) drives the backlight GPIO high; `display_backlight_brightness` (line 671) steps PWM0's CSR duty (bits 23:16)
  for an LCD panel, and uses DCS 0x51 only for the OLED.
- Ours has the WLED GPIOs (`kWledPwm` V0, `kWledEnable` V1, `TegraDisplayPower.cpp:72`) driven low then enable high in
  power up, and the pin put back to PWM0 with pull down in power down (`:504`). It has no PWM0 programming at all: the
  only PWM code is the fan's PWM1 in `SwitchThermal.cpp:566-601`, which is off on the Erista. Nothing sets the duty,
  so an LCD backlight would stay at whatever the bootloader left.
- `drivers/SwitchBrightness/TegraPanel.cpp` is DCS only and `sc7Display` panics for any other panel. The brightness
  API (0..2047 candela) has no LCD back end.
- Needed to close it: a PWM0 back end behind `tegra_panel_set_brightness` (clock, CSR duty, pin function), the
  backlight enable on resume, and the `sc7Display` gate widened for the INL panel's suspend and resume.

Unknown
- Whether this code lights a real P062CCA-AZ1. It has never run on one.
- Whether the first boot re-initializes the display or adopts what hekate and U-Boot left lit. If it adopts, the
  backlight gap does not matter for a first boot.
- Touch and the panel's brightness range for the ADT are not looked at.

Held: writing the PWM back end and widening `sc7Display` are small but untestable without the panel, so they wait.

## 2. generate_adt: what is still OLED only

Confirmed (`tools/package/native_oled_adt.py`)
- Per board: root `model`, root `compatible`, the `product` node strings and the `arm-io` `device_type` (the SoC, the
  last compatible entry). `IDENTITIES['erista']` matches the pinned `erista-sku0.dtb` root
  (`tools/package/test/boot_script_board_test.py:110-116`).
- Everything else is board independent or OLED: the OLED output is byte identical (golden SHA-256 in
  `native_oled_adt_board_test.py`).
- The generated tree has only `chosen`, `memory-map`, `cpus`, `psci`, `defaults`, `options`, `product`, `arm-io` with
  the GIC. It names no panel, touch, backlight, PMIC or SD node, so the display and storage drivers do not get their
  hardware from the ADT today. That is why the tree has no Erista specific content beyond the identity.

Inferred
- Anything an Erista build adds later (panel, backlight, touch, a different Wi-Fi/Bluetooth module) has to be added per
  board when a driver starts reading it. Nothing reads it yet.
- The function and file names say OLED. Renaming is not needed for a first Erista boot and would touch golden tests.

## 3. SD card and storage on T210

Confirmed
- `NativeSwitchBlock.cpp` drives SDMMC1 at 0x700B0000 (`kControllerPA`), the same controller on T210 and T210B01. It
  refers to hekate's bdk for the SDR104 bring up and tuning.
- The SD root path is a feature independent of the board table. Switching SE2 off does not affect it (see NOTES.md).

Unknown, needs a console
- hekate's `bdk/storage/sdmmc_driver.c` has T210 specific tuning and clock choices. I have not shown that ours
  matches them on a T210, and the answer cannot be had without running the controller on one.
- The 1.8 V rail switch and the SD power GPIO path on the Erista PMIC are shared with the OLED in the code I read, but
  never run on an Erista.

Held: ODM and SD tuning on device.

## 4. Read only diagnostic mode (design, not built)

Goal: read the 25 MC registers `boot/loader/loader.c` `native_gate` compares (0x70019050 to 0x70019d54) as the loader
would see them, without touching the normal gate or the normal boot path.

Confirmed
- The pinned `bazel/pinned/firmware/bl33.bin` (U-Boot) contains the `md` help text ("memory display") and "reset", so
  `md.l` exists. The normal script uses `itest.l`, `setexpr.l`, `mw.b`, `cp.b`, `fatsize`, `load`, `fdt`, `echoe`,
  `sleep` and `setenv stdout vidconsole` (`boot/u-boot-script.txt`), so those also exist.
- The script is a template (`boot/u-boot-script.txt`) filled by `tools/package/boot_script.py` and sourced from a slot
  in bl33.bin. A second template makes a second bl33.bin without changing the first.
- The loader itself reads these 25 registers from the same non-secure world before it decides (`native_gate`), so a
  non-secure read is permitted on an OLED at that point. `dump.sh` reads them the same way under Linux.
- The normal script refuses a board whose row has `boot 0` (`board_gate`), which is why the Erista cannot run it.

Plan
1. `boot/u-boot-diag-script.txt`: keep the normal script's console check (`stdout vidconsole`, so values appear on the
   LCD without a serial cable). Take the board from U-Boot's own `t210b01` and `sku` and refuse anything but a table
   row, as now. Then one `md.l <addr> 1` per register, in `native_gate`'s order, and stop in a loop. No `mw`, `cp`,
   `load`, `fatwrite`, `source`, `bootm`, `booti`, `go`.
2. `boot_script.py` gets a `diag` mode that fills this template, with no `@LOADER@`, `@EXPORTER@` or kernel slots.
3. A test that parses the produced script and fails if any command is not on a whitelist (`md.l`, `echo`, `echoe`,
   `setenv`, `test`, `if`, `else`, `fi`, `while`, `sleep`, `base`, `true`), and that its address list equals
   `native_gate`'s table (read from `loader.c`).
4. Run it on the OLED first. It must print `native_gate`'s expected values. Only then run it on an Erista.
5. Reaching it: a separate hekate entry that boots the diagnostic bl33, so the normal entry and `boot 0` do not change.

Unknown
- Whether U-Boot's `md.l` survives a locked register. Step 4 shows it on the OLED, and the script stops at
  the first refusal as `dump.sh` does.
- Whether the Erista has the same register layout as `native_gate`'s table. The values are what we want to learn.
- Whether hekate hands U-Boot a lit screen on the Erista with the LCD console. The script's own `CONSOLE FAILED`
  loop covers the failure.

## 5. Unconditional hardware writes in the minimal mode

After this change the board thread starts only what the board's features allow, except mc_errors and the watchdog.
Line by line:

| Writer | What it writes | State now | Erista risk |
| --- | --- | --- | --- |
| mc_errors (`TegraMCErrors.cpp:158`, `TegraMCErrors.h:32-38`) | MC INT_STATUS write one to clear of its error bits, then INT_MASK for 8 error bits | always on | low: MC is the same block, error bits only. Kept: it is the way to see SMMU faults on a first boot |
| watchdog (`NativeSwitchWatchdog.cpp:16`) | no hardware: enable, disable and pet touch nothing; WDT4 only in debug kernels | always on | none. Kept: launchd's exec waits for `switch_watchdog_ready` |
| GPIO eager start (`TegraGPIO.cpp:244-248`) | INT_ENB 0 and INT_CLR 0xff on every port | now BUTTONS | removed from minimal. GPIO still starts when a driver asks for a pin interrupt or output (`:595`, `:659`) |
| SwitchButtons (`SwitchButtons.cpp:198-232`) | PMC_CNTRL INTR_POLARITY (bit 17), MAX77620 ONOFF_IRQ_MASK, IRQ_TOP_MASK, INT_EN_LBT, GPIO debounce on the volume pins | now BUTTONS | removed from minimal. It also panics if bit 17 does not stick, which is unverified on a T210 |
| `switch_pmic_thermal_take` | MAX77620 RTC_UPDATE0, ALARM2 | now THERMAL | removed from minimal |
| `switch_pmic_mark_start` | RTC alarm | DEVELOPMENT and RTC_MARK | off |
| `moduleClockOn` | MAX77620 CNFG1_32K, AME_GPIO | WIRELESS | off |
| RTC publish (`SwitchPMIC.cpp:773`, mode 1) | none: mode 1 never writes the time back | always on | none |
| PMIC halt and restart hooks | PMIC and PMC writes only when the user powers off or restarts | always on | needed for a clean power off |
| I2C5 adopt (`TegraI2C.cpp:543-555`) | CNFG bits only if they differ from what U-Boot left | always on | low |
| SD attach | CAR clock and reset, SDMMC1 registers, pad PMC, pinmux Schmitt (below) | always on | see SD below |
| display | CAR, DSI, DC, MIPI_CAL, PMC pads, LCD GPIOs (section 1) | always on | needed to see anything |

SD cold path (`NativeSwitchBlock.cpp`)
- Confirmed: setup talks to the MAX77620 LDO2 (`kPmicLdo2` 0x27): `switchTo1V8` writes 1.8 V after CMD11 (`:3541`),
  and the SC7 paths write off and 3.3 V (`:3896-3937`). It also toggles card power GPIO PE4, PMC IO power and the
  SDMMC1 pad detect bits, and sets Schmitt triggers in APB_MISC pinmux.
- Confirmed: `switch-no-sd-uhs` keeps the card at the 3.3 V mode U-Boot left (header comment, line 9). That leaves no
  voltage write on the first boot, only the controller setup.
- Inferred: LDO2 at 0x27 on the Erista is the same SD rail as on the OLED, since the Erista also uses a MAX77620.
  Not verified, and it is a voltage, so nothing is changed here.
- Recommendation, for you to decide: pass `switch-no-sd-uhs` on the first Erista boot. It needs no code change.

Not done
- Display, SD controller and panic save were read for their writes, not audited line by line for ordering.

## 6. Changes made for this section, and their effect on the OLED

- `switch_pmic_thermal_take` runs only with THERMAL. The OLED has THERMAL, so its behaviour is unchanged.
- New feature bit BUTTONS (bit 16) gates TegraGPIO's eager start and SwitchButtons. `SWITCH_FEATURES_ALL` is now
  0x1ffff and the OLED has it, so the OLED starts both as before.
- The Erista's features stay 0. Neither change lifts `boot 0` or touches a voltage or memory setting.

## 7. First boot and later

Needed for a first boot (a console that reaches launchd and shows a screen)
- Board gate and MC carveout check with Erista values (needs the loader's own reading), `boot 1` set by a person.
- ADT with the Erista identity (done), DTB pinned (done).
- SD root volume read through SDMMC1 (with `switch-no-sd-uhs` suggested), raw APFS.
- A visible display: either adopt the lit panel or the INL init (exists, untested). Panic screens depend on it.
- PMC reset and the panic record, MC error reporting, the watchdog service (launchd waits for it).
- PMIC on I2C5 for RTC reads and power off.

Can wait until after a first boot
- Backlight control (PWM0 back end), touch, sensors, ALS, motion, wireless, audio, GPU, video, USB, charger.
- Buttons, which are the only way to wake the screen, so they are the first thing to add after boot.
- THERMAL and EMC (fan and DRAM clock). Safety note: without THERMAL nothing controls the fan or CPU rate beyond what
  U-Boot left, so keep the first boots short and watch the temperature.
- SLEEP, CACHE_IDLE and RTC_MARK. SE2 stays off for good, the Erista has no second security engine.
