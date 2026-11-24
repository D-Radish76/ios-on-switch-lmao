# Erista (T210) port notes

Target: original Switch (HAC-001, T210, LCD panel). Source hardware: OLED (Aula, T210B01).
Nothing here has been built with Bazel or run on hardware. "Confirmed" means read in source or checked by script.

## Confirmed

- hekate runs `bpmpfw.bin` on Erista too (`l4t.c`), so BL31 starts on both chips.
- Display power-up and power-down in `TegraDisplayPower.cpp` now branch on HIDREV major (1 = T210) using hekate's T210 tables (`TegraDisplayErista.h`). The 43-entry JDI panel table matches hekate `di.inl` entry for entry.
- The file passes `g++ -std=c++20 -fsyntax-only -Wall -Wextra` against stub headers. It is 614 lines, over the 500-line guideline.
- Nothing reviewed writes fuses or touches eMMC (U-Boot `boot_targets=mmc1`). PMIC writes in the loader are read, verify, write, read back. DFLL returns ENODEV instead of guessing.
- `sc7Display()` in `NativeLegacyDisplay.cpp` panics for any panel other than AMS699VC01 (0x2050), so the new LCD code is unreachable until `TegraPanel` and `SwitchBacklight` support the LCD.
- hekate leaves its framebuffer lit at handoff (`sld_type=1`), so the first screen needs no new display code.
- The boot gate is `t210b01=1` / `sku=3` in `boot/u-boot-script.txt`. `native_gate` in `loader.c` compares MC carveout registers with values read from the OLED.
- `osfmk/arm64/cpu.c` hardcodes `nintendo,aula` and `NintendoSwitchOLED`.

## Unverified

- Erista values of `t210b01` and `sku` in U-Boot (expected 0 and 0). Needs a hekate "Console info" photo.
- The Erista sku-0 DTB. Source: `nx-plat.dtimg` entry 0, or `/sys/firmware/fdt` under Switchroot Linux.
- Whether the MC carveout values in `native_gate` hold on Erista.
- BPMP ping and EMC DVFS table match on Erista (`emc.c` only raises EMC when both match).
- CPU rail: Erista uses MAX77621 at I2C5 0x1B, not MAX77812 at 0x31/0x33. DFLL needs a MAX77621 path.
- BQ24193 charge parameters (tuned for the OLED battery, max charge voltage 4208 mV), thermal and fan logic, and I2C5 address collisions are not reviewed.
- Touch: the OLED uses FTM4CD60. The Erista touch controller is not identified.
- Every register in the Erista display path. The tables are from hekate, but the runtime order and sleep times are untested.

## ios-switch (urielc918)

Facts only. It boots iOS 16 (xnu-8796) on Erista through a custom BL33 shim. The repo is MIT with APSL-2.0 XNU patches; this repo is GPL-2.0, so no code is copied.

## Debugging without UART

- Loader refusals print `LOADER REFUSED` with a code on screen, and U-Boot prints `REFUSED: <status>`.
- pstore keeps the panic record across a reboot (`SwitchPanicSave.h`).
- `SwitchDiagHud` draws a debug HUD in DEVELOPMENT and DEBUG builds.

## Next

1. Get the DTB and the Console info photo.
2. Make the boot gate table-driven.
3. Handle `nintendo,aula` in the ADT and kernel.
4. Adjust `native_gate`, BPMP/EMC and the DFLL/MAX77621 path.
5. Add LCD `TegraPanel` and backlight, then lift the SC7 panel gate.
6. Touch.

No boot test until code review and a Bazel build are done.
