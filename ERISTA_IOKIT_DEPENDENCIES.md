# Erista: from VM init to launchd, driver dependencies and hardware status

Source reading of 2026-11-27. No code or image changed, nothing run, `boot` 0, feature mask 0, first XNU run on HOLD. Paths are `kernel/xnu-10063.141.1` (iOS 17) unless a repository path is given. Marks: **confirmed** (read in the file named), **inferred**, **unknown**. The earlier stages are in `ERISTA_XNU_BOOT_STAGES.md`.

## The path after `arm_vm_init`

| Step | Function and source | Notes |
| --- | --- | --- |
| 1 | `arm_init` continues: `switch_diag_init`, `switch_panic_save_init`, `switch_diag_vm_done` (`osfmk/arm/arm_init.c:548-553`) | **confirmed**. With `switch_stop_after_vm` the run ends here (`ERISTA_FIRST_XNU_BOOT.md`) |
| 2 | `PE_create_console`, `PE_init_printf`, `PE_init_platform(TRUE, ...)` (`arm_init.c:600-621`) | the interrupt controller side of the platform expert; the GIC-400 words were only read on the Erista |
| 3 | `machine_startup` then `kernel_bootstrap` then `kernel_bootstrap_thread` (`arm_init.c:688`, `osfmk/kern/startup.c:696-939`) | scheduler, clock service, `hv_support_init`; bodies not read here |
| 4 | `PE_init_iokit` (`startup.c:820-821`, `pexpert/arm/pe_init.c`) | creates the IOKit root; the platform expert class is matched by the ADT root (`IONameMatch device-tree`, `drivers/ArmDTPlatform/Info.plist:13-14`) |
| 5 | `bsd_init` (`startup.c:939`; `bsd/kern/bsd_init.c`) | `IOKitInitializeTime` at `:699`, which calls `switch_pmic_start` (`iokit/Kernel/IOStartIOKit.cpp:93`) |
| 6 | `bsd_autoconf` then `IOKitBSDInit` (`bsd_init.c:808`, `:1131`), `switch_boot_iokit_bsd_ready` | starts IOService matching, then late display probe (`kernel/switch/boot/bsd_init.c`) |
| 7 | `switch_boot_autoconf_done` calls `switch_root_boot_prepare` (`bsd_init.c:809`; `diagnostics/switch/SwitchRootBoot.c:99`) | **confirmed**. Publishes the SD block device (`switch_block_publish`), the NVRAM controller, and starts the board driver thread (`switch_board_start`) |
| 8 | `IOFindBSDRoot` then `vfs_mountroot` (`bsd_init.c:896-902`; `iokit/bsddev/IOKitBSDInit.cpp:764`) | waits for the root device, see below. A failed mount panics (`switch_boot_mountroot_done`) |
| 9 | `vm_init_before_launchd`, `load_init_program` (`bsd_init.c:1098-1110`) | launchd is `/sbin/launchd` from the mounted root (**inferred** from XNU's usual `init_program_name`; the Switch override was not read) |

## The SD card and the root file system

- **The SD driver is not matched from the ADT.** `NativeSwitchBlock` is an `IOService` published directly by `switch_block_publish` (`drivers/NativeSwitchBlock/NativeSwitchBlock.cpp:4725`, `registerService` at `:4747`), and it runs only after IOBSD exists. The reason is written in the code: APFS's container start and its IOBSD-matched service take the same lock in opposite order, so publishing the disk earlier deadlocks the two matching threads (`kernel/switch/boot/bsd_init.c`, comment at `switch_boot_autoconf_done`). **Confirmed.**
- `setup()` (`NativeSwitchBlock.cpp:4445`) reads the GPT and needs exactly one APFS or named partition. With none the device is never published, every write is refused, and `IOFindBSDRoot` waits 30 s (`IOKitBSDInit.cpp:795`, `waitForMatchingService(matching, 30 * kSecondScale)`) and then panics `Failed to mount root device` after `switch_raw_root_wait_diagnostic` (`:1019`). The wait for the root itself was not timed in detail (`ERISTA_FIRST_XNU_BOOT.md`).
- Root matching: `IOFindBSDRoot` reads `root-matching` (XML) from `/chosen` (`IOKitBSDInit.cpp:823`); the ADT's `/chosen root-matching` is "as the OLED's, board independent" (`ERISTA_ADT.md`). **Confirmed** that the Erista tree carries the same property; whether it matches the partition the Erista card carries was not run.
- Controller access: SDMMC1 at `0x700B0000`, same on T210 and T210B01. The 1.8 V and UHS path is skipped when `HIDREV` major is below 2 (`NativeSwitchBlock.cpp:3715`); the Erista's `00022117` gives 1, so it stays at the speed hekate left. Tap delay and trim differ (Erista 4 and 2, OLED `0xb` and `0xe`, `ERISTA_NATIVE_PORT_PLAN.md` A-3), taken from the Erista DTB. What is measured: SDMMC1 clock and reset readable at EL1 (D0-S v2) and U-Boot's SD driver reading sector 0 (D1-min). XNU's driver on the Erista has not run (**unknown**).
- Unconditional card writes: the CMD49 pair around `checkCq`, which changes the card's cache and CQ registers, never user data (`ERISTA_FIRST_XNU_BOOT.md`, section "The SD driver").

## How drivers start, and what depends on what

Two mechanisms exist and the second matters more here.

1. **ADT and IOKit matching** (few classes): `ArmDTPlatform` (`IONameMatch device-tree`), `ArmCpu` (`IOProviderClass IOPlatformDevice`), the GIC through the platform expert. The Erista ADT is a fixed minimal tree; only the nodes in the table of `ERISTA_ADT.md` are allowed by a test, and drivers do not read panel, touch, PMIC or tuning data from it. **Confirmed** (`ERISTA_ADT.md`, "What the tree is").
2. **Direct start functions on one board thread** (`switch_board_thread`, `diagnostics/switch/SwitchRootBoot.c`), each guarded by a board feature bit. The order in the file is the dependency order stated in its comment: Bluetooth first (publishes the BD address), MC error interrupt before any DMA client, the watchdog before launchd reads LaunchEvents, SE2 early, Joy-Con after Bluetooth, sensors after their sources. With the Erista mask at 0 only the entries with feature 0 run: `NativeSwitchWatchdog` (`switch_watchdog_publish`). **Confirmed.**

| Order | Start function | Feature bit | Depends on | Erista mask 0 |
| --- | --- | --- | --- | --- |
| 1 | `switch_bluetooth_start` | WIRELESS | MAX77620 32 kHz clock | off |
| 2 | `tegra_mc_errors_start` | MC_ERRORS | the GIC, MC registers | off |
| 3 | `switch_watchdog_publish` | none | no hardware in a release kernel (`ERISTA_BOOT_AUDIT.md`) | **runs** |
| 4 | `tegra_se_start` | SE2 | SE2 (T210B01 only) | off, and absent on the Erista |
| 5 | `switch_joycon_start` | WIRELESS | Bluetooth | off |
| 6 | `switch_usb_start` | USB | XUSB, PD part | off |
| 7 | `tegra_gpio_start` | BUTTONS | the GIC | off |
| 8 | `switch_thermal_start` | THERMAL | I2C1, TMP451, SOC_THERM, PWM1, DFLL, CPU rail | off |
| 9 | `switch_battery_start` | CHARGER | I2C, the GIC | off |
| 10 | `switch_sensors_start` | SENSORS | thermal and others | off |
| 11 | `switch_touch_start` | TOUCH | I2C or SPI, GPIO | off |
| 12 | `switch_buttons_start` | BUTTONS | GPIO, MAX77620 IRQ masks | off |
| 13 | `switch_als_start`, `switch_motion_start` | ALS, MOTION | I2C | off |
| 14 | `switch_wlan_publish` | WIRELESS | the module's clock, SDMMC or PCIe link (**unknown** which) | off |
| 15 | `tegra_gm20b_probe` | GPU | GPU rail, clocks, `TegraSMMU` | off |
| 16 | NVDEC, NVENC, VIC, NVJPG | VIDEO | the GPU side clocks | off |
| 17 | `switch_audio_publish` | AUDIO | codec, I2S, clocks | off |
| after | `switch_suspend_start` | SLEEP | PMIC | off |

`switch_pmic_start` and `tegra_mc_errors_start` run from `IOKitInitializeTime` and the board thread behind their own bits (`PMIC` bit 17, `MC_ERRORS` bit 18), which the Erista row does not have. **Confirmed** (`ERISTA_ADT.md`, "PMIC differences").

Dependency edges that are visible in the code or comments and worth keeping:

- Disk before IOBSD deadlocks APFS: publish after `bsd_autoconf` only.
- The board thread is separate so the root mount never waits behind a driver that sleeps on I2C or waits for the GIC.
- A driver whose interrupt is missing panics with its own reason; with mask 0 none of those is reached.
- Without THERMAL nothing controls the fan after hekate's `fan_set_duty(0)` (`ERISTA_NATIVE_PORT_PLAN.md`).
- Display: `native_legacy_display_late_probe` runs after IOBSD, because IOSurface's personality needs the genuine IOBSD resource (`kernel/switch/boot/bsd_init.c`).

## Hardware status for the Erista

Classes: **common** (the same code and the same hardware, usable), **needs a change** (code exists but is shaped for the OLED or never ran on an Erista part), **not implemented**, **unknown** (nothing read decides it). "Measured" means a run on the Erista; none of the entries is run under XNU.

| Hardware | Class | Basis |
| --- | --- | --- |
| Framebuffer adoption (`0xF5A00000`, 720x1280, `a8b8g8r8`) | common, with the assumption unprinted | the loader's check passed on the Erista by control flow (`ERISTA_NATIVE_PORT_PLAN.md` row 4); stage 3 maps it |
| LCD panel initialisation (InnoLux P062CCA-AZ1) | needs a change | `TegraDisplayErista.h` and `TegraDisplayPower.cpp` carry hekate's T210 tables, never run on this panel; `sc7Display` panics for any panel but the OLED's (`NativeLegacyDisplay.cpp:5344`); the first boot can adopt what hekate lit, so this may not be needed first (`ERISTA_RESEARCH.md` sections 1 and 10) |
| Backlight | not implemented | no PWM0 back end (clock, duty, pin function), and `TegraPanel.cpp` is DCS only; with an adopted display the backlight stays as hekate left it (**inferred**; not run) |
| Touch panel | needs a change | `SwitchTouch` is an FTM4/fts driver (names in the source); the Erista's controller is **unknown** here; the ADT's OLED touch description must not be used (`ERISTA_NATIVE_PORT_PLAN.md`) |
| SD card (SDMMC1) | common controller, needs a change for tuning, unverified | above; the first boot can use `switch-no-sd-uhs` |
| USB (XUSB) | unknown | same XUSB; charger and PD parts differ by board; `SwitchUSB` not compared |
| PMIC | needs a change | MAX77620 at `0x3c` is common (RTC, power off, reset). The CPU and GPU rails differ: Erista MAX77621 at `0x1b` and `0x1c`, OLED MAX77812 at `0x33`. `SwitchPMIC` only knows the OLED's. No rail change at the first boot |
| Battery and charger | unknown | `SwitchBattery` names BQ24193 and MAX17050; the Erista's parts are not checked against it |
| Cooling and temperature | needs a change | TMP451 at I2C1 `0x4C` read (ID `55`, T1 v3; one-shot, T2). Mariko's remote offset is -12.5 C, the Erista's is 0; fan tachometer and 5 V handling differ (`ERISTA_THERMAL.md`); the driver carries the Mariko values. SOC_THERM trip, DFLL and the CPU rail are not touched; no hardware protection is shown (T3, T4 not built) |
| Multi-core | unknown | PSCI `CPU_ON` for cores 1 to 3 never ran on the Erista; the first boot uses one core (`--//boot:smp=false`). `PSCI SYSTEM_RESET` is measured |
| GPU (GM20B) | needs a change, unknown | same GM20B; clock and rail differences between T210 and T210B01 (`TegraGM20B/gpu_clock.c:131`, `gpu_power.c:187`; `TegraDFLL/platform.c:695` refuses anything but a T210B01 A01); the Apple graphics stack is not started with the bit off. A crash in Apple code later is a missing contract on our side, not a reason to stub it (`CLAUDE.md`) |
| Wi-Fi and Bluetooth | unknown | `SwitchWLAN` and `SwitchBluetooth` name Broadcom BCM435x parts; the Erista module and its 32 kHz source were not compared |
| Audio | unknown | `SwitchAudio` names Realtek RT5639/RT5640; codec wiring is said to differ (`ERISTA_NATIVE_PORT_PLAN.md`), not compared in detail |
| Security engine | common (SE), SE2 absent | the Erista has `se@70012000` only; SE2 stays off for good |
| Memory controller and DRAM | common | 25 MC words equal the OLED's, banks measured; EMC scaling off |
| Interrupt controller, timer | common | GIC-400 words read, CNTFRQ 19.2 MHz measured; no interrupt delivery tested |
| Watchdog | needs a decision | `NativeSwitchWatchdog` runs on every board without hardware in a release kernel; the TKE watchdog study is `ERISTA_T4A_DESIGN.md` |

## What this changes for the next steps

1. The first XNU boot needs none of the rows above except the framebuffer, DRAM and the CPU; it stops at VM done by design.
2. A boot to launchd needs the SD driver on the Erista card, a root file system the loader's SD reference accepts, and the single-core ADT. Everything else stays behind the feature bits.
3. The first features worth adding, in the plan's order: buttons (the only way to wake the screen), the read-only temperature display, then THERMAL. Each is a reviewed bit.

## Sources

`kernel/xnu-10063.141.1/bsd/kern/bsd_init.c`, `osfmk/arm/arm_init.c`, `osfmk/kern/startup.c`, `iokit/Kernel/IOStartIOKit.cpp`, `iokit/bsddev/IOKitBSDInit.cpp`; `kernel/switch/boot/bsd_init.c`; `diagnostics/switch/SwitchRootBoot.c`; `drivers/NativeSwitchBlock/NativeSwitchBlock.cpp`, `drivers/ArmDTPlatform/Info.plist`, `drivers/ArmCpu/Info.plist`; `kits/SwitchBoard/SwitchBoard.h`; `ERISTA_ADT.md`, `ERISTA_NATIVE_PORT_PLAN.md`, `ERISTA_RESEARCH.md`, `ERISTA_THERMAL.md`, `ERISTA_FIRST_XNU_BOOT.md`, `ERISTA_BOOT_AUDIT.md`.
