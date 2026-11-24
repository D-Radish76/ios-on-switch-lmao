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

Unknown
- Whether this code lights a real P062CCA-AZ1. It has never run on one.
- Backlight: `drivers/SwitchBrightness/TegraPanel.h` has no LCD path in what I read. The LCD backlight (PWM pin
  `kPinmuxLcdBlPwm`, the PMIC/backlight enable) is unreviewed.
- Touch and the panel's brightness range for the ADT are not looked at.

Held: lifting the `sc7Display` gate and any backlight code need a console.

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

## 4. Diagnostic mode for the MC carveouts

Goal: read the 25 MC registers that `boot/loader/loader.c` `native_gate` compares (0x70019050 to 0x70019d54) at
loader time, without opening the normal gate.

Confirmed
- `native_gate` refuses every non-OLED board and refuses on any register mismatch. `tools/mc_carveout/dump.sh` reads
  the same registers under Switchroot Linux with BusyBox `devmem`, 32 bit reads only. Linux time values are a
  reference, not loader time values.

Options, in the order I recommend
1. A U-Boot script variant that runs `md.l` over the register list and stops. A test would check the script contains
   only a whitelist of commands (`md.l`, `echo`, `reset`/`poweroff`) and no `mw`, `cp`, `bootm`, `booti`, `go`, `source`.
   Run it on an OLED first and compare to the `native_gate` table: that proves the method before it is trusted on an
   Erista. The normal script and gate stay untouched.
2. A separate loader build with a dump only entry. Larger, and it reuses code that sits on the boot path.
3. A hekate payload. Different codebase, nothing shared with ours.

Unknown: whether U-Boot's `md.l` at those addresses is allowed on a console with the MC locked down. Option 1 answers
that on the first run, and the dump must stop on the first refusal as `dump.sh` does.

## 5. Unconditional hardware writes in the minimal mode

The board thread (`diagnostics/switch/SwitchRootBoot.c`) starts only the features the board table allows, plus four
that always run: mc_errors, watchdog, gpio, buttons. Writers I read:

| Writer | Registers | Gated? | Erista risk |
| --- | --- | --- | --- |
| `SwitchPMIC.cpp` start | PMIC reads, `moduleClockOn` (WIRELESS gated) | partly | none known |
| `switch_pmic_thermal_take` (`SwitchPowerMark.cpp:282`, called at `SwitchPMIC.cpp:781`) | MAX77620 RTC_UPDATE0, and ALARM2 if a record is found | no, runs on every board that answers on I2C5 | low: same PMIC family, but it is a PMIC write the table does not control |
| `switch_pmic_mark_start` | RTC alarm | yes: DEVELOPMENT/DEBUG and RTC_MARK | off on Erista |
| `tegra_i2c_init` bringUp (`TegraI2C.cpp:538`) | CAR clock enable, pad config, pinmux for I2C3/4, bus config. I2C5 already clocked by U-Boot is adopted without a reset | no | I2C5 path is read only on adoption except CNFG bits; other buses are only touched when something opens them |
| mc_errors, watchdog, gpio, buttons | MC error registers (reads and interrupt enables), watchdog, GPIO config | no | not audited line by line; GPIO/button pin maps are Switch wide in what I read |

Findings
- Confirmed: `switch_pmic_thermal_take` is not behind `SWITCH_FEATURE_THERMAL`. It is a small RTC write on every board.
- Inferred: harmless on an Erista, because the Erista also uses a MAX77620 at the same address. Not verified.
- Not done: gating it. That is a behaviour change on the OLED path, so it needs your decision.
- Not audited: the SD attach cold path writes, and the always on drivers line by line.

## 6. Bazel on an M1 Mac

See `BUILD_CHECKLIST.md`, "Before you start".
