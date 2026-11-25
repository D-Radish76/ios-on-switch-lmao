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
- DRAM: Erista trains the MTC table with Minerva, trains 800 MHz, then hekate's `hw_deinit` runs `minerva_deinit` and DRAM is back at 204 MHz when BL31 and U-Boot start. The table goes to BL31 (`emc_table_base`). Mariko puts the tables into the BPMP-FW DTB and starts at 1866 or 2133 MHz through ARC. DRAM voltage is written only with `ram_oc` keys, which `ios.ini` does not set.
- Erista patches the SC7-Exit firmware with the console's RSA modulus read from eMMC (read only). Mariko uses `pkg1_warmboot_config`.
- Carveouts use one layout. Only the LP0 scratch registers that save them differ.
- Both chips get `PMC_SCRATCH201 = BIT(1)`, which marks MAX77621 or MAX77812 as the CPU PMIC.

Where this repo assumes T210B01 (all fail safe or are untested on Erista):

- `emc.c` raises EMC only after a BPMP ping and a table match, so with no BPMP it should leave DRAM at 204 MHz, where hekate leaves it. It checks VDD_CORE against the T210B01 table, which does not apply to Erista.
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

## D0 loader destination (checked from files, not on the console)

- Confirmed in the pinned `bl33.bin`: its control FDT has no reservations and a placeholder `/memory` of 6 GiB; this U-Boot
  has no overlap message for `bootm loados`; `fdt rsvmem print` and `bdinfo` only print.
- Confirmed in `erista-sku0.dtb` and `oled-sku3.dtb`: memreserve `0x80000000..0x80020000`, `ramoops_carveout`
  `0xB0000000..0xB0200000`.
- Done: `check_destination` now refuses a loader span that overlaps a reservation of the DTB it copies. Script unchanged.
- Not verified: free memory at `0xA0000000` on an Erista beyond the OLED analogy; DTB copy equal to the pinned file.
  Details in `ERISTA_LOADER_DIAG.md`, "Pre-checking the destination".

## D0-pre memory diagnostic (built and tested on Linux, not run on a console)

- `iOS Diag Memory` and `iOS Diag Memory FDT` print `bdinfo` and the control FDT's memory nodes, then reset after 15 s.
- Confirmed from the pinned `bl33.bin`'s machine code: `bdinfo` only reads `gd`/`bd` and prints. `fdt addr` with a size
  rewrites the blob (`fdt_open_into`), so the scripts use the address alone; a test forbids the other form.
- `fdt` behaviour is read from upstream v2018.01/v2024.01 sources.
- Erista run: page 2 worked (`/memory` = 6 GiB placeholder `0x80000000`+`0x180000000`, no `/reserved-memory`, empty reserve map). Page 1 hit a hush syntax error from an apostrophe in an `echo`; fixed and linted (`hush_lint.py`), not yet re-run. Both the script's reset and the failed-script fallback reset returned to hekate on the Erista.
- `fdt_ranges.py` now refuses every malformed or unsupported DTB (header version, bounds, overlap, FDT_END, hierarchy,
  cells, ranges, 64 bit overflow); `fdt_ranges_test` covers each case.

## Erista DRAM and U-Boot placement (D0-pre page 1, measured)

- Banks: `0x80000000`+`0x7FD00000` and `0x100000000`+`0x80000000`; total 4093 MiB (4 GiB less 3 MiB).
- The 3 MiB at `0xFFD00000..0x100000000` are the secure carveouts (GPU firmware, GPU WPR, secure firmware, TZDRAM): from the
  run's MC rows and hekate's `l4t.c`. Register roles are decoded, not documented; U-Boot's own derivation is unread.
- U-Boot relocated to `0xFFC87000`, TLB `0xFFCF0000`, stack top and FDT `0xFF87F710`/`0xFF87F728`: 1.5 GiB above the D0 loader.
- No overlap between `0xA0000000..0xA00151D0` and any listed range (`memory_layout_test`). Not a proof that unlisted memory is
  free. Details, margins and open items in `ERISTA_LOADER_DIAG.md`.

## Before the first boot test

- Keep the SD card as the only write target and boot from hekate, so Linux and hekate stay the fallback.
- Charge the battery first. hekate shows no screen at low battery unless VOL+ and VOL- are held.
- Do not run the OLED loader on Erista before the boot gate is table-driven. It refuses on `WRONG_BOARD`, which is the expected result.

Research findings that need no console: `ERISTA_RESEARCH.md`.

Changes that touch OLED code paths (neither changes what the OLED does): thermal record read gated by THERMAL; BUTTONS feature bit (16) for the GPIO eager start and SwitchButtons. See `ERISTA_RESEARCH.md`, section 6.

Read only diagnostic (own hekate entry, no iPadOS): `boot/diag/README.md`.

Bazel test result, Mac (Bazel 9.3.1, Xcode 27.2 27B52, commit c790f50e4): `bazel test //kits/SwitchBoard/test:all //boot/loader/test:all //tools/package/test:all` passed, 18 of 18, with the real GoogleTest. Details in `BUILD_CHECKLIST.md`. Erista stays `boot 0`.

Bazel build, Mac (commit eb0758e6): `bazel build //:boot --config=ios17 --jobs=4 --verbose_failures` completed, 151 actions. Fixes on the way: codesign output in the error, and links that leave the Sileo bundle copied as files before signing. Details in `BUILD_CHECKLIST.md`. Next phase: the read only diagnostic on the Erista, after review (`boot/diag/README.md`, "On the Erista"). Erista stays `boot 0`; no iPadOS boot.

Mac, commit fb0b61ef6: `//tools/package/test:all` 7 of 7 pass, diagnostic package generated (bl31 45 KB, bl33 455 KB, script 6.9 KB, ini 155 B). Not started on hardware. Remaining risks before the first run: `boot/diag/README.md`, "Before the first run". Erista stays `boot 0`.

Mac, commit 24136381: `//tools/package/test:all` 7 of 7 pass, `diag_boot_test` passes. Card hashes of `libsys_minerva.bso`, `libsys_lp0.bso`, `bpmpfw.bin`, `sc7entry.bin` and `sc7exit.bin` equal those of the hekate 6.5.5 release zip. `strings | grep -iE 'therm|temp|tmon|throt|fan'` on the card's `bpmpfw.bin` printed nothing. The wait before reset is now 15 s, set with `--dwell` (`boot/diag/README.md`, "Is the wait needed"). Erista stays `boot 0`; no iPadOS boot.

First Erista diagnostic run (commit c6931169, hekate 6.5.5, Nyx `More configs`): `t210b01=0 sku=0`, `status=READY console=OK`, `DONE: all 27 registers read`, 15 s wait, `reset` landed in hekate's main menu on its own, no freeze or loop, about 21 s on video. This confirms `reset` goes to hekate on this console with this BL31. Register values are not in the repository yet; comparing them with `native_gate` is next. Temperature protection is still unverified. Erista stays `boot 0`.

First run compare, Erista: all 25 `native_gate` MC registers equal the OLED values (25 same, 0 DIFFERENT, 0 NOT READ). HIDREV `0x00022117` (T210, major 1), FUSE_RESERVED_ODM4 `0x00000204`. Saved as `boot/diag/erista-first-run.log`, with the 25 rows written from the table (the run reported them equal), not typed from the video. `native_gate` writes nothing, so no MC write is needed. Whether the loader sees the same at its own moment, every gate classified, the loader diagnostic design and the order of work to the first XNU start: `ERISTA_LOADER_DIAG.md`. Erista stays `boot 0`.

D0 loader diagnostic implemented (not built on the Mac yet, not run on hardware): `boot/diag/loader/README.md`, design in `ERISTA_LOADER_DIAG.md`. Own loader, `bl33.bin` and hekate entry (`iOS Diag Loader`); no kernel window, exporter or `loader_enter_kernel`; MC registers only read; 15 s on the CPU counter, then PSCI `SYSTEM_RESET` (unverified from the loader). Host checks here: 33 Python tests, 14 C++ tests with a stand-in gtest (not the real GoogleTest), and a real clang and `ld.lld` build of the loader through the inspection. The first run's measured log replaces the table-derived fixture with `diag_boot.py adopt`. Erista stays `boot 0`.

Mac verification of the D0 loader, reported 2026-11-25 (commit `d7f6f75a` onward): related Bazel tests all pass, `//boot:diag_loader` builds and passes inspection, package SHA-256 values match the reviewed ones. Taken from the user's report; the output was not seen here. Inputs unchanged since `e2c03a58`. Not run on hardware. Run steps and stop rules: `boot/diag/loader/RUN_CHECKLIST.md`. Open at the first run: PSCI `SYSTEM_RESET` from EL1, the event stream waking `wfe`, the EL2 to EL1 step and drawing on the Erista, thermal protection with the fan off, and memory near `0xA0000000` that no source lists. Erista stays `boot 0`.

D0 loader, first run on a physical Erista (2026-11-25, card files hash-matched): passed. The reviewer transcribed the photograph IMG_1016.JPG; the text is `boot/diag/erista-d0-run.log` and `tools/package/test/d0_run_test.py` reads it (11 tests pass on Linux, not run under Bazel). Header: `HIDREV 00022117`, `ODM4 00000204`, `BOARD ERISTA T210B01=0 SKU=0 BOOT=0`, `EL1 STATE PASS`, `CNTFRQ=19200000`, entry SP `FF87D010`. The 25 MC rows equal `mc_table.h` in order and all read SAME. The six GIC words are those of a GIC-400 (decoded in `ERISTA_LOADER_DIAG.md`); the repo holds no OLED values for them, so they are not compared with any. The FDT walk gave code 0, ALL RANGES FREE, 3 reserved nodes, 2 banks, 3 reserve-map entries. Hekate came back by itself, which is the first time PSCI `SYSTEM_RESET` from the loader at EL1 and the `wfe` wake have been seen to work. Not shown by this run: thermal protection (the fan was off), memory outside the DTB's listed ranges, the origin of the third reserve-map entry (one comes from the DTB, one from the script, one is unattributed). Design for D1, not started: `ERISTA_D1_DESIGN.md`. Erista stays `boot 0`.

Approved 2026-11-25 after the D1 design review: D1-pre (`boot/diag/sd/`, entries `iOS Diag SD` and `iOS Diag SD Info`), D0-R (`//boot:diag_loader_r`, entry `iOS Diag Loader R`) and temperature research (`ERISTA_THERMAL.md`). Implemented, not built on the Mac, not run on hardware. Linux checks: `diag_sd_test`, `diag_loader_test`, `hush_lint_test`, `exporter_audit_test`, `d0_run_test` pass; `reservations_test` passed against a stand-in for GoogleTest only (the real one runs on the Mac); the D0 and D0-R loaders compile and link with Linux clang and lld and pass `diag_loader_inspect`, and `diag.c` with the flag off compiles to the same object bytes as before D0-R. Findings: the pinned U-Boot's control FDT disables I2C1 and the PWM block (no TMP451 or fan access from U-Boot); `mmc info` before `mmc dev 1` would start the internal storage (`mmc0` is `sdhci@700b0600`); hekate's fan, TMP451 and 5 V code has Erista branches this repository's Mariko driver lacks; the exporter writes only its own 24 KiB and the 256 byte record and forms no device address. `mc_verified` is a separate change, not made. `boot` stays 0.
