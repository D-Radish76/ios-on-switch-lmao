# Erista ADT

`tools/package/native_erista_adt.py` makes the Apple DeviceTree for the Erista. It shares only the serialization with the
OLED's (`native_adt_common.py`), and the OLED's tree stays byte for byte what it was (all 32 profiles compared with the
previous generator). Nothing here boots: the Erista row is `boot 0` and its feature mask is 0.

## What the tree is

A fixed minimal description. It is not a conversion of the Erista's DTB, and no DTB property is copied into it. The
drivers do not read panel, touch, PMIC, SDMMC tuning or security engine data from the ADT, they carry their own register
addresses and values, gated by the board's feature bits (`SwitchRootBoot.c`). So the DTB matters here in two ways: it is
the evidence for the few numbers the ADT does hold, and it shows which driver assumptions need an Erista version.

A test (`native_erista_adt_test.py`) allows only the nodes listed in the table below. A new node fails it until someone
audits it.

## What the tree holds, and who reads it

| ADT | Value | Evidence | Read by | Status |
| --- | --- | --- | --- | --- |
| `/` model, compatible | `Nintendo Switch (2017)`, `nvidia,odin`, `nintendo,icosa`, `nintendo,nx`, `nvidia,tegra210` | root of the pinned DTB, checked by test | `switch_board_resolve` (compatible witness), MobileGestalt | matches the DTB |
| `/arm-io` device_type | `nvidia,tegra210` | last compatible entry | ArmDTPlatform | matches |
| `/arm-io` ranges, `/arm-io/intc` reg | GICD `0x50041000`/`0x1000`, GICC `0x50042000`/`0x100` | DTB `/interrupt-controller` reg, GIC-400 words read by D0 | `ArmGIC.cpp:330-346`, `ArmCpu.cpp:359` | matches the DTB. They read only the distributor frame and its size (at least `0x1000`) |
| `/arm-io/intc` compatible | `arm,cortex-a15-gic` | DTB | ArmGIC match | matches |
| `/cpus/cpu0` | reg 0, state running, `arm,cortex-a57`, timebase 19.2 MHz | DTB `/cpus/cpu@0` and `/timer` clock-frequency; CNTFRQ and MIDR read by D0 | `pe_identify_machine.c:64-70` (state and timebase), `ArmCpu.cpp:329` (reg) | matches. Only the running core is read for the timebase |
| `/psci` | absent | single core | `ArmCpu.cpp` PSCI conduit (`psciConduit`) | no secondary core is started |
| `/chosen` dram-base, dram-size | `0x80000000`, 4 GiB | both measured banks lie inside it (`memory.c:205` checks the exact banks) | `osfmk/arm/arm_init.c:584-593` | same as the OLED, and the banks are equal |
| `/chosen` switch-sd-reference, switch-ram-ledger, root-matching, nvram proxy | as the OLED's | board independent | `SwitchSD.c:594`, loader `memory.c`, IODTNVRAM | not Erista specific |
| `/chosen` random-seed | pinned, mixed by the loader | board independent. The Erista has no SE2, so the loader keeps the pinned seed | `pe_gen.c:178` | open: every Erista boot would start from the same seed |
| `/memory-map`, `/defaults`, `/options`, `/product` | as the OLED's | board independent | arm_vm_init, XNU | not Erista specific |

## DTB against the tree

What the Erista DTB has and the tree does not, and why that is right for a first boot. The full per-block classification
is in `ERISTA_NATIVE_PORT_PLAN.md`, A-3.

| DTB | Tree | Reason |
| --- | --- | --- |
| four A57, `enable-method psci` | one A57 | PSCI `CPU_ON` is not measured on this BL31 |
| `/timer` `arm,armv8-timer`, `/timer@60005000` | timebase on the CPU node only | the architected timer needs no node here; the Tegra timer block is unused (unverified that nothing needs it) |
| MC, EMC, SMMU, PMC | none | drivers map them by address |
| `sdhci@700b0000` tap 4, trim 2 | none | see below |
| max77621 at 0x1b and 0x1c, max77620 at 0x3c | none | drivers hardcode their I2C5 addresses |
| single `se@70012000` | none | SE is not a node the drivers read |
| panel, touch, pwm-fan, soctherm, XUSB | none | features 0 |

## What `adt_edit.py` adds

The package runs the tree through `adt_edit.py` (iPad identity and the nodes userland asks for). That step is shared with
the OLED. Run here over the Erista tree without `--smp` against a synthetic Apple tree (the real one needs the IPSW, so
the Mac run is Phase N3's), it adds `machine-timeouts`, `maps`, `audio`, `bluetooth`, `wlan`, `usb-device`, `usb-xhci`,
`buttons` and `filesystems`, keeps one core and no `/psci`, and `adt_check.py` passes. Its OLED assumptions:

| Added | Assumption | Status for the Erista |
| --- | --- | --- |
| `/arm-io/bluetooth`, `/arm-io/wlan` | BCM4356A3 patchram, J120's identity | module and clock source not compared: unverified. Drivers are not started (WIRELESS off) |
| `/arm-io/usb-device`, `usb-xhci` | iPad identity, `tegra210` compatible | neutral names. Not started (USB off) |
| `/buttons` | J120's GPIO bytes, presence only is read | presence only. Not started (BUTTONS off) |
| `/chosen` display-boot-rotation 270 (portrait) | the 720x1280 panel rotated | the Erista framebuffer is also 720x1280 (hekate), but its rotation property was not read: unverified |
| mac addresses, unique-chip-id | software values | not Erista specific |
| four cores (`--smp`) | PSCI | must not be used for the Erista |

## SDMMC tap and trim

The tree carries no tap or trim value, and none is needed for a first boot. `NativeSwitchBlock.cpp:3715` skips the whole
UHS re-initialisation when HIDREV major is below 2 ("not T210B01"), so on an Erista the controller keeps the setup hekate
and U-Boot left. The tap and trim writes (`setTiming` at `:3480`, the resume path at `:3948`) sit behind that check, and
the resume path only runs after sleep.

The DTB's `tap-delay 4`, `trim-delay 2` (OLED `0xb`, `0xe`) are Linux driver values. Their register meaning for this
driver is not established, so they are not copied. If Erista UHS is wanted later the values go in the board table next to
the board's row, taken from a measurement of the Erista's own sampling window, not from this DTB. Before the SLEEP
feature is enabled on the Erista, the resume path needs the same board check (it writes B01's `kTrimB01`).

## PMIC differences

The ADT has none. The drivers do, and these are the ones that act on every board:

| Driver | Before | Now |
| --- | --- | --- |
| `SwitchPMIC.cpp:756` (`switch_pmic_start`, called from `IOStartIOKit.cpp:92`) | Configured I2C5 (divisor `(5<<16)\|1`, bus clear), probed MAX77812 at `0x33` and `0x31` (`SwitchClocks.h:64`), read the MAX77620 RTC, installed the halt and restart hook | Behind the `PMIC` feature. The Erista has no MAX77812 in its DTB, and none of this has run on one |
| `TegraMCErrors.cpp:124` (`tegra_mc_errors_start`) | Wrote MC_INTMASK | Behind the `MC_ERRORS` feature |
| `NativeSwitchWatchdog` | Starts on every board | Stays. No hardware in a release kernel. See `ERISTA_BOOT_AUDIT.md` |

MAX77620 at `0x3c` is on both boards and covers RTC, power-off and reset. The CPU and GPU rails differ (Erista max77621,
OLED max77812) and only `SwitchThermal` and `SwitchPMIC` know about them, so they stay behind the THERMAL bit.

## Open

- The watchdog is the one driver still started on every board. Its open questions are in `ERISTA_BOOT_AUDIT.md`.
- The Erista tree has not been through the real Apple tree, the loader's layout step or a Bazel build (Phase N3, below).
- `random-seed` repeats on every Erista boot because the loader only mixes SE2's output.

## Verifying the single-core build (Mac, boot stays 0)

Nothing below boots anything. Run each under `--config=ios17` and `--config=ios27`.

```
bazel test --config=ios17 //boot/loader/test:carveout_test //tools/package/test:carveout_fixture_test \
  //tools/package/test:native_erista_adt_test //tools/package/test:native_oled_adt_board_test \
  //tools/package/test:boot_script_board_test //tools/package/test:adt_check_test \
  //tools/package/test:erista_adt_edit_test //tools/package/test:adt_media_test \
  //tools/package/test:diag_boot_test //tools/package/test:diag_loader_test //tools/package/test:d0_run_test \
  //tools/mc_carveout:test_dump
bazel build --config=ios17 //boot:adt //boot:erista_adt //boot:package
python3 tools/package/adt_check.py bazel-bin/boot/erista-full-ram.adt --cores 1
```

What each answers, and what is expected:

| Check | Expected |
| --- | --- |
| `erista_adt_edit_test` | passes on both kinds: the real Apple tree gives one core, no `/psci`, no OLED strings, the Erista's board entry kept |
| `adt_check.py` on `erista-full-ram.adt` | `ADT OK (1 core(s))`; with `--cores 4` it must fail |
| `shasum -a 256 bazel-bin/boot/full-ram.adt` | the same as built from `85757fc66` in a worktree: the OLED's tree did not change |
| the seven diagnostic images | the hashes in `boot/diag/erista-diag-image-hashes.txt`, built as in `ERISTA_T1V3_RUN.md` section 1. They do not compile `loader.c` or `carveout.h`, and `mc_table.h` changed in a comment only |
| the normal loader (`//boot:package`) | its `loader.bin` is expected to differ from `85757fc66`: `native_gate` now calls `carveout.h`. Compare the OLED's behaviour by `carveout_test`, not by hash |
| `--//boot:smp=false` for the OLED | an OLED package without `--smp` must still build; it is the OLED's own single-core path |

## The Erista package (Phase N5)

`//boot:erista_package` is `//boot:package`'s rule with the Erista's tree (`//boot:erista_adt`), its DTB, one core
(`//boot:single_core`) and its own output directory `erista-ios`. Everything else comes from the same attribute set
(`boot_entry` in `boot/package.bzl`: a BUILD file cannot unpack a dictionary into a call), so the two cannot drift apart unseen. It is `manual`. Build it with
`--//install:prebuilt=local`: the release's prebuilt loader and window were made for the OLED, and bl33 stage refuses them.

Building it does not let anything boot. The Erista row is `boot 0`: U-Boot's script has no clause for it (WRONG_BOARD) and
the loader refuses at `loader.c:292` before `native_gate`. `erista_package_test` pins the target's attributes; the refusals
are `boot_script_board_test` and `carveout_gate_test`.

`//boot:erista_loader_all_rows` links the loader with every row's MC values (`-DSWITCH_CARVEOUT_ALL_ROWS=1`), as it would be
once the Erista row boots. It exposes `loader.uboot.bin` only, built from a private `manual` package target, and exists
for the linker's stamps ASSERT. Compiled here with a stub `layout.h`: +96 bytes over the normal loader (rodata +100, text
-4). The linked margin is for the Mac to report.

Mac commands, iOS 17 (`--config=ios17`):

```
bazel test --config=ios17 //tools/package/test:erista_package_test //kits/SwitchBoard/test:all \
  //drivers/SwitchPower/test:switch_board_start_source_test //boot/loader/test:carveout_gate_test
bazel build --config=ios17 //boot:package                  # the OLED's, as before
bazel build --config=ios17 //boot:erista_package           # single core, Erista tree, bazel-bin/boot/erista-ios
bazel build --config=ios17 //boot:erista_loader_all_rows   # links, or the stamps ASSERT fires
shasum -a 256 bazel-bin/boot/full-ram.adt                  # the same as from 85757fc66
```

`--//boot:smp=false` for the OLED is still its own path. Nothing here sets `boot 1`.
