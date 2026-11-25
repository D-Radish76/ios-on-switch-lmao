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
- The SD power GPIO path (PE4) and the LDO2 recovery writes are shared with the OLED in the code I read, but never
  run on an Erista. The 1.8 V switch is skipped on a T210 (section 9).

Held: ODM and SD tuning on device.

## 4. Read only diagnostic mode (built)

`boot/diag/` and `tools/package/diag_boot.py`; its README has the build, run and stop steps. Facts it rests on:

Confirmed
- The pinned `bl33.bin` has the `md` command ("memory display" help text), and the normal script already uses
  `setenv stdout vidconsole`, `itest.l`, `setexpr.l`, `echoe`, `sleep` and `fdt`.
- The loader reads the same 25 registers from the same non-secure world before it decides (`native_gate`).
- hekate finds U-Boot's DTB through the word at bl33+0x10 (`bootloader/l4t/l4t.c:161-162`), so the diagnostic keeps
  U-Boot whole and adds only the script slot.
- The diagnostic image is U-Boot plus the script slot: no loader, exporter, DTB or window, so it cannot reach iPadOS.
  The script ends in a loop and the ini sets `scan_dev_for_extlinux=true`, so a failed script does not fall into an
  extlinux boot either. The normal entry, its ini and its `boot` flag are untouched.
- Tests (`diag_boot_test.py`, 12, run here with `unittest`): command whitelist, forbidden commands, one literal aligned
  `md.l` per address, the address list equal to `native_gate`'s table plus HIDREV and ODM4, the final loop, the image
  layout, the ini, and `compare`. A mutation (`mw.l` added to the script) fails the command test.

Unknown
- Whether `md.l` survives a register the MC refuses. The script prints a line per read, so a stop shows which one.
- Whether hekate lists `iOS Diag` under `More configs` as I expect from its ini handling. Not run.
- The tests ran with Python's `unittest`, not Bazel. The Bazel entries are written, not built.

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
- Confirmed: on a T210 the 1.8 V switch is skipped by the HIDREV check, so there is no LDO2 write at boot even
  without `switch-no-sd-uhs`; section 9 has the lines.
- Inferred: LDO2 at 0x27 on the Erista is the same SD rail as on the OLED, since the Erista also uses a MAX77620.
  Nothing is changed here.

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

## 8. Hardware writes on the whole U-Boot path (the diagnostic and the normal boot)

Read from the sources I have: hekate's `l4t.c`, `hw_init.c`, `di.c`, our script and our tools. U-Boot's own source is
not in the workspace, only its binary, so its init is described by what we observe and not by line.

| Stage | Writes | Evidence | For the Erista |
| --- | --- | --- | --- |
| hekate, before launch | display and panel init, SD at SDR104 (it reads bl33 that way), minerva trains the DRAM table and sets 800 MHz, PMC scratch (panel id, BL31 params), BL31 and BPMP firmware setup | `l4t.c:735-805`, `:1105-1146`, `boot/README.md` | the same as every Switchroot Linux boot of that console. Inferred: that boot works on yours |
| hekate DRAM voltage | `max7762x_regulator_set_voltage` only if `ram_oc_vdd2` or `ram_oc_vddq` is set | `l4t.c:182-187`, `:782-785` | neither ini sets them, and a test checks the diagnostic's ini has no `ram_oc` key |
| ATF (bl31.bin) | secure monitor setup | pinned file, same hash as the Erista's own (earlier BL31 comparison) | unchanged |
| U-Boot init | its own SD, clock and console setup | binary only | not audited: no source. It is the Switchroot U-Boot every Switchroot boot uses |
| diagnostic script | none: `setenv`, `echo`, `test`, `sleep` touch RAM or the screen, `md.l` reads | the test | none |
| normal script | RAM only (`mw.b` of the args page, `cp.b` of the DTB, `fdt` edits), then the exporter and loader | `boot/u-boot-script.txt` | not run: `board_gate` refuses `boot 0` rows |

So the diagnostic adds no hardware write of its own. Everything before it is the stock Switchroot path.

## 9. SD voltage

Confirmed
- hekate's SDR104 read of bl33 and U-Boot's own SD setup happen before our code. Our `switch-no-sd-uhs` cannot change
  them. The kernel's SD driver says the card arrives at 3.3 V high speed (`NativeSwitchBlock.cpp:5-8`).
- The kernel skips the 1.8 V switch on a T210 without the boot arg: `setup()` checks `HIDREV >> 4 & 0xf < 2` and skips
  with "not T210B01" (`NativeSwitchBlock.cpp:3715`), before any PMIC read. So on an Erista there is no LDO2 write at
  boot, with or without `switch-no-sd-uhs`. The arg is a second guard, not the only one.
- Voltage writes that remain in the driver, none at boot on an Erista: `resetCard` (a card power cycle after a stuck
  command queue, `:2448-2456`) and suspend and resume (`powerOffCard`, `powerOnController`, `:3896-3937`). They switch
  LDO2 off and to 3.3 V, GPIO PE4 and the PMC pad bits. Suspend needs SLEEP, which the Erista does not have.

Unknown
- The error recovery path has never run on an Erista. Nothing here changes a voltage value: both writes are 3.3 V or
  off, the state the card already has.

## 10. Taking over hekate's framebuffer and backlight

Confirmed
- hekate's framebuffer is 720 by 1280 at 0xF5A00000 (`bdk/memory_map.h:98-99`), the one the normal script checks
  (`handoff_fb_*`), and the script says it never dereferences it: U-Boot's console draws into it as a simple
  framebuffer from the control FDT.
- The backlight is not touched by U-Boot's path in anything we read: no `display_backlight*` call exists between
  hekate's `launch_l4t` and the handoff in `l4t.c`, and the script has none.
- `hw_deinit(keep_display)` skips `display_end` when the display is kept (`bdk/soc/hw_init.c:473-478`). Whether hekate
  keeps it for an L4T launch is `launch_l4t`'s `hw_deinit(ctxt->sld_type)` argument, with `sld_type` set true by
  default (`l4t.c:865`, passed at `:1168`): inferred to mean seamless display, so the screen stays lit.

Unknown, and the first thing the diagnostic shows
- Whether the Erista's LCD is lit and showing the U-Boot console. If it is, the first iPadOS boot can adopt it and the
  PWM back end in section 1 is not needed for a first boot.
- Our display driver's power up (`TegraDisplayPower.cpp`) re-initializes the display. Whether it should skip that on
  the Erista when the panel is already lit is not decided: it would be a driver change and needs the panel.

## 11. A safe first boot with THERMAL off

Confirmed
- Without THERMAL nothing of ours reads a temperature or sets the fan or the CPU rate: "XNU does no DVFS, so the A57
  cluster otherwise stays where hekate left it" (`SwitchThermal.cpp:~40`), and the fan driver is that file's.
- hekate's own `hw_deinit` stops the fan (`fan_set_duty(0)`), puts the TMP451 in shutdown and powers the 5 V rails
  down, but only under `BDK_HW_EXTRA_DEINIT`, which is Nyx's build (`bdk/soc/hw_init.c:481-490`). Which hekate binary
  launches the entry decides which of these happened.
- The OLED figures in that file (998.4 MHz PLLX, 800 mV) are the T210B01 path. The T210's CPU rate and voltage at
  handoff are not in anything I read.

Conditions I suggest for the first boots (a recommendation, not a measurement)
1. Run the diagnostic first. It uses the CPU only for U-Boot's `sleep 1` loop.
2. Short boots, a person present, the console on a hard surface, battery charged, no charger or dock changes.
3. Power off at the first sign of a hot back or a fan that is not turning. Do not leave a boot running to see how far
   it gets.
4. After a first boot works, the first thing to add is a read only temperature display (TMP451 on I2C1, reads only,
   no fan write), before THERMAL itself.

Addendum, checked against hekate `8e1392d` (the bootloader build, not Nyx)
- `bootloader/main.c` and `l4t/l4t.c` contain no `fan_set_duty`, `tmp451_*` or `soc_therm` use. `fan_set_duty` appears in
  `bdk/soc/hw_init.c:487` under `BDK_HW_EXTRA_DEINIT` only. The Nyx GUI reads the TMP451 (`gui.c:1376`, `:2533`).
- No source in hekate programs `SOC_THERM` trip points. The hardware trip is therefore not something this project can
  say is armed at handoff.
- Consequence: a launch from hekate guarantees no fan duty, no temperature reading and no CPU rate limit for the code
  that follows. For the diagnostic that means a short run by hand (`boot/diag/README.md`).
