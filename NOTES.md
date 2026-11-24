# Erista (T210) port notes

Target: original Switch (HAC-001, T210, LCD panel). Source hardware: OLED (Aula, T210B01).
Nothing here has been built with Bazel or run on hardware. "Confirmed" means read in source or checked by script.

## Confirmed

- The Erista DTB (`bazel/pinned/firmware/erista-sku0.dtb`, nx-plat.dtimg entry 0, `tegra210-odin.dts`, 168,190 bytes, SHA256 `e6b1b64543f9980d2ce38c4848ec0eb2a702048282002beee6924560fd6f1c84`) was read: model `Nintendo Switch (2017)`, compatible `nvidia,odin`, `nintendo,icosa`, `nintendo,nx`, `nvidia,tegra210`. The board table's Erista row equals it. It comes from a Switchroot card, not from a console this session had.
- The pinned `bl31.bin` is the file on a Switchroot SD card: its full SHA256 equals `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4`. The user hashed it on the card; the card's file was not seen here. That makes it the same build ios-switch runs on an Erista. It does not show that the pinned U-Boot's PSCI/SMC use works on an Erista, which only a boot test shows.
- hekate's L4T launcher starts BL31 on both chips (`l4t.c`). That says nothing about whether the pinned `bl31.bin` supports Erista; see "BL31 on Erista".
- U-Boot sets `t210b01` and `sku` itself (disassembly of the pinned `bl33.bin`): `t210b01` is 1 when HIDREV major (0x70000804 bits 7:4) is 2, else 0. `sku` is 0 when the major is not 2. On T210B01 it follows FUSE 0x7000F9D8 bits 19:16: 2 gives 2, 4 gives 3, anything else 1. An Erista should therefore report `t210b01=0`, `sku=0`. Not run on hardware.
- Display power-up and power-down in `TegraDisplayPower.cpp` now branch on HIDREV major (1 = T210) using hekate's T210 tables (`TegraDisplayErista.h`). The 43-entry JDI panel table matches hekate `di.inl` entry for entry.
- The file passes `g++ -std=c++20 -fsyntax-only -Wall -Wextra` against stub headers. It is 613 lines, over the 500-line guideline.
- Nothing reviewed writes fuses or touches eMMC (U-Boot `boot_targets=mmc1`). The loader's `emc.c` only reads the MAX77620 SD0 rail. The PMIC writes in `SwitchThermal.cpp` and `TegraGM20B/platform.c` are read, verify, write, read back. DFLL returns ENODEV unless it finds a T210B01 A01.
- `sc7Display()` in `NativeLegacyDisplay.cpp` panics for any panel other than AMS699VC01 (0x2050), so the new LCD code is unreachable until `TegraPanel` and `SwitchBacklight` support the LCD.
- The boot gate is `t210b01=1` / `sku=3` in `boot/u-boot-script.txt`. `native_gate` in `loader.c` compares MC carveout registers with values read from the OLED.
- `osfmk/arm64/cpu.c` hardcodes `nintendo,aula` and `NintendoSwitchOLED`.

## Unverified

- Power-down order against hekate `_display_panel_and_hw_end`: panel reset low, 10 ms, LCD AVDD off, BL_PWM pad to PWM0 with pull-down, 10 ms, then clocks off. This matches. hekate never clears WLED enable (V1) there; the backlight is turned off before by driving V0 low (`display_backlight(false)`). Ours leaves V1 alone as well. hekate also hands V0 back to the PWM controller (`gpio_config` SPIO), which `TegraGPIO` has no call for, so V0 stays a GPIO output. Backlight ownership belongs to the future LCD `SwitchBacklight`.
- Boot gate design: accept a board only when `t210b01`, `sku` and the DTB model agree with a table row (OLED 1/3, Erista expected 0/0 from the Console info photo). A row without a verified DTB stays refused.
- hekate leaves its framebuffer lit at handoff, so the first screen needs no new display code. This comes from hekate's L4T launcher and ios-switch's notes (framebuffer 0xF5A00000, 720x1280). The U-Boot script checks the same address and format on the OLED.
- The values `t210b01=0` and `sku=0` on a real Erista. The Console info photo only checks the hardware type.
- The Erista sku-0 DTB. Source: `nx-plat.dtimg` entry 0, or `/sys/firmware/fdt` under Switchroot Linux.
- Whether the MC carveout values in `native_gate` hold on Erista.
- BPMP ping and EMC DVFS table match on Erista (`emc.c` only raises EMC when both match).
- CPU rail: Erista uses MAX77621 at I2C5 0x1B, not MAX77812 at 0x31/0x33. DFLL needs a MAX77621 path.
- BQ24193 charge parameters (tuned for the OLED battery, max charge voltage 4208 mV), thermal and fan logic, and I2C5 address collisions are not reviewed.
- Touch: the OLED uses FTM4CD60. ios-switch reports an STM FTS4 at I2C3 0x49 (chip 0x3670) on Erista. Not confirmed on this console.
- Every register in the Erista display path. The tables are from hekate, but the runtime order and sleep times are untested.

## BL31 on Erista

What was read: `bazel/pinned/firmware/bl31.bin` (45,655 bytes, stripped, SHA256 `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4`), hekate `bootloader/l4t/l4t.c`, and upstream TF-A `plat/nvidia/tegra/soc/t210` through a mirror. The repo has no BL31 source. The source is `CTCaer/switch-l4t-atf` (named in `SwitchSuspend.cpp`); its branch `switch-v5-atf2.6` `plat/nvidia/tegra/soc/t210/plat_setup.c` asserts `tegra_chipid_is_t210()` and branches on `tegra_chipid_is_t210_b01()`, so the source covers both chips. Which commit built the pinned binary is not known, and the binary was not disassembled. ios-switch records only the abbreviated hash `dea0ecd4...e2b4` for its own `bl31.bin` (the card's `switchroot/ubuntu-noble/bl31.bin`, Noble 5.1.2), and this repo's pin comment names the same Noble 5.1.2 entry. The full SHA256 of the file on a Switchroot card was later reported equal to ours.

T210B01 specific handling in the BL31 path (upstream TF-A, and `plat_setup.c` of the switchroot branch above):

- SE2 is mapped only on T210B01. `tegra_se_save_tzram` and `tegra_se_resume` run only there.
- On T210 the PMC security bit is set again in `plat_late_platform_setup` and `tegra_pmc_resume` runs on cluster idle exit.
- `plat_supports_system_suspend` is true on T210B01 and, on T210, only when hekate hands over an SC7-Entry firmware.
- `plat_late_platform_setup` powers the BPMP off when an SC7-Entry firmware is present, and hekate passes one on T210 only. After BL31 starts, an Erista has no BPMP to answer `MRQ_PING`, `MRQ_CLK` or `MRQ_EMC_DVFS_TABLE`.
- Upstream asserts `tegra_chipid_is_t210()` in `plat_early_platform_setup`. Release builds compile that out.

What hekate does differently per chip before it starts BL31:

- Erista needs `bootloader/sys/l4t/` to hold `sc7entry`, `bpmpfw.bin` and `sc7exit`, and needs Minerva. Without Minerva hekate stops with "Minerva missing". Mariko loads `bpmpfw_b01.bin` and `mtc_tbl_b01.bin`. The existing Switchroot Linux install should provide the Erista set; this is not checked on the card.
- DRAM: Erista trains the MTC table with Minerva, sets 800 MHz and hands the table to BL31 (`emc_table_base`). Mariko puts the tables into the BPMP-FW DTB and starts at 1866 or 2133 MHz through ARC. DRAM voltage is written only with `ram_oc` keys, which `ios.ini` does not set.
- Erista patches the SC7-Exit firmware with the console's RSA modulus read from eMMC (read only). Mariko uses `pkg1_warmboot_config`.
- Carveouts use one layout. Only the LP0 scratch registers that save them differ.
- Both chips get `PMC_SCRATCH201 = BIT(1)`, which marks MAX77621 or MAX77812 as the CPU PMIC.

Where this repo assumes T210B01 (all fail safe or are untested on Erista):

- `emc.c` raises EMC only after a BPMP ping and a table match, so with no BPMP it should leave DRAM at 800 MHz. It checks VDD_CORE against the T210B01 table, which does not apply to Erista.
- `TegraBPMP.c` returns `-ENODEV` without a mailbox.
- `TegraDFLL` returns `-ENODEV` unless the chip is T210B01 A01, before any clock or rail change. The CPU then stays at whatever clock BL31 leaves, which costs speed but no safety.
- `SwitchThermal.cpp` and `TegraGM20B/platform.c` probe MAX77812 at I2C5 0x31 or 0x33. Erista has MAX77621 at 0x1B, so those reads should NACK and stop before any write. Not run.
- The BQ24193 limit of 4208 mV was tuned for the OLED battery. Check it against the HAC-001 battery before any charger write is allowed.

Open checks before a boot test:

- The commit and build options of the pinned `bl31.bin`.
- The Erista SD0 voltage and the VDD_CORE needed at 800 MHz.

## ios-switch (urielc918)

Reported, not re-run here. It boots iOS 16 (xnu-8796) on Erista through a custom BL33 shim instead of U-Boot, and its notes list the MAX77621 CPU rail at I2C5 0x1B and SD0 at 1125 mV. The repo is MIT with APSL-2.0 XNU patches; this repo is GPL-2.0, so no code is copied.

## Debugging without UART

- Loader refusals print `LOADER REFUSED` with a code on screen, and U-Boot prints `REFUSED: <status>`.
- pstore keeps the panic record across a reboot (`SwitchPanicSave.h`).
- `SwitchDiagHud` draws a debug HUD in DEVELOPMENT and DEBUG builds.

## Next

1. Read the MC carveout registers from Linux on the Erista with `tools/mc_carveout/dump.sh` as a reference. `native_gate`'s Erista values wait for the loader's own reading, because Linux's view is not guaranteed to match it.
2. Adjust BPMP/EMC and the DFLL/MAX77621 path.
3. Add LCD `TegraPanel` and backlight, then lift the SC7 panel gate.
4. Touch.

Done: the board table, the table-driven boot gate, the board profile in boot-args, per-board model strings and `generate_adt(board=)`.

No boot test until code review and a Bazel build are done.

## Before the first boot test

- Keep the SD card as the only write target and boot from hekate, so Linux and hekate stay the fallback.
- Charge the battery first. hekate shows no screen at low battery unless VOL+ and VOL- are held.
- Do not run the OLED loader on Erista before the boot gate is table-driven. It refuses on `WRONG_BOARD`, which is the expected result.

Research findings that need no console: `ERISTA_RESEARCH.md`.

Changes that touch OLED code paths (neither changes what the OLED does): thermal record read gated by THERMAL; BUTTONS feature bit (16) for the GPIO eager start and SwitchButtons. See `ERISTA_RESEARCH.md`, section 6.
