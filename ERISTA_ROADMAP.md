# Erista roadmap

Facts live in `NOTES.md`. This file holds what to do next and the design for it. Nothing here is built with Bazel or tested
on hardware, and the boot gate, voltage settings and boot test stay untouched until a reviewer and a Mac build say so.

## Findings

### BL31

- Ours: `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4`, 45,655 bytes.
- The full SHA256 of `bl31.bin` on a Switchroot SD card equals ours (hashed by the user, not seen here), so it is the build
  ios-switch's card carries. It does not show that the pinned U-Boot's PSCI/SMC use works on an Erista.
- Source: `CTCaer/switch-l4t-atf`, branch `switch-v5-atf2.6`. `plat_setup.c` supports T210 and T210B01. The commit behind the
  pinned binary is unknown.

### U-Boot `t210b01` and `sku`

Found by disassembling `bazel/pinned/firmware/bl33.bin` (board late init, next to "Board was not initialized properly!"):

| Variable | Rule |
|---|---|
| `t210b01` | `1` if HIDREV (0x70000804) bits 7:4 equal 2, else `0` |
| `sku` | `0` if that major is not 2. Otherwise FUSE 0x7000F9D8 bits 19:16: 2 gives `2`, 4 gives `3`, else `1` |
| `display_id` | PMC secure scratch 113 low 16 bits, as hekate wrote it |

An Erista gives `t210b01=0`, `sku=0`. This matches switchroot's `boot.scr`, where sku 0 is ODIN (Erista), 1 MODIN, 2 VALI, 3 FRIG.

### Erista DTB

Extracted by the user from `nx-plat.dtimg` entry 0 on a Switchroot card (Android dt_table, magic `0xD7B7AB1E`; entry 3 is
`oled-sku3.dtb`). Pinned as `bazel/pinned/firmware/erista-sku0.dtb`: 168,190 bytes, FDT version 17, SHA256
`e6b1b64543f9980d2ce38c4848ec0eb2a702048282002beee6924560fd6f1c84` (read here and equal to the reported hash), source
`tegra210-odin.dts`. Its root says model `Nintendo Switch (2017)` and compatible `nvidia,odin`, `nintendo,icosa`,
`nintendo,nx`, `nvidia,tegra210`. The Erista row (`Nintendo Switch (2017)`, `nintendo,icosa`) and `generate_adt(board='erista')`
equal it, and a test checks both pinned DTBs against their rows. The OLED's DTB has the same shape (`nvidia,fric`,
`nintendo,aula`, `nintendo,nx`, `nvidia,tegra210b01`).

No boot entry chains the file, and the row is still `boot 0`. The rest of the Erista ADT is still the OLED's: the arm-io bus
is only the GIC, but the display, touch and the storage probe are not audited for the HAC-001.

### MC carveout values

`native_gate` compares 25 registers (`0x70019050`, `0x70019648` and others up to `0x70019d54`) with values read on the
OLED. Nothing in the repository or the DTB gives the Erista's. The OLED DTB's `/reserved-memory` holds only the ramoops
region (`0xb0000000`, 2 MiB) and two empty framebuffer carveouts that hekate fills in, so the pinned Erista DTB will not
show the MC security carveouts either.

A reference without a boot test of this project: read the 25 words from Switchroot Linux on the Erista, read only, with
`tools/mc_carveout/dump.sh` (steps in its README). It calls `busybox devmem <address> 32` and nothing else, prints each
value next to the OLED's and stops if the kernel refuses `/dev/mem`. Run it on the OLED too, from the same Linux, to see
how far Linux's view differs from the loader's there.

What the dump cannot do: Linux reads the registers late in the boot, the loader reads them early, and nothing guarantees
the carveouts are the same at both moments. Several equal Linux dumps do not turn the Linux values into the loader's.
Update: U-Boot's script read all 25 on an Erista and they equal the OLED's (`ERISTA_RESEARCH.md`, section 13). The loader's own
moment is still unread, and a read only loader diagnostic can read it without `boot 1` (`ERISTA_LOADER_DIAG.md`), so the dump is not needed.

Register names for the 25 addresses are not mapped yet (Linux `tegra210` MC and hekate's
`mc.h` are the sources).

### Where the OLED is assumed

| Area | Where | OLED assumption | On Erista today |
|---|---|---|---|
| Boot gate | `boot/u-boot-script.txt` | `t210b01=1`, `sku=3` | Refuses (`WRONG_BOARD`) |
| MC carveouts | `boot/loader/carveout.h`, called from `native_gate` | One table per board; the Erista table is the D0 measurement | Refuses on a mismatch; an Erista never reaches it while `boot 0` |
| EMC raise | `boot/loader/emc.c`, `TegraBPMP`, `SwitchPMIC` | BPMP answers, T210B01 table, VDD_CORE rows | No BPMP after BL31 (SC7-Entry firmware makes it power the BPMP off), so ping fails and nothing is written. DRAM stays at hekate's 800 MHz |
| CPU rail and clock | `TegraDFLL/platform.c`, `SwitchThermal.cpp` | MAX77812 at I2C5 0x31/0x33 | DFLL returns `-ENODEV`. SwitchThermal refuses on "not a T210B01 (HIDREV)" before any rail access |
| GPU | `TegraGM20B/platform.c` | VDD_GPU on MAX77812 | Not safe to run; off in the minimal mode |
| Charger | `SwitchBattery`, `SwitchChargePolicy.h` | BQ24193 limits from Horizon's Aula battery table (4208 mV) | Writes REG00, REG01, REG05; limits not checked against the HAC-001 battery |
| USB-PD | `SwitchUSB/SwitchPD.cpp` | BM92T36 at I2C1 0x18, GPIOs PK4, PK5, PX3 | Erista part and wiring not identified |
| Touch | `SwitchTouch` | FTM4CD60, I2C3 0x49, firmware writes | ios-switch reports an STM FTS4 at the same address; a write would reach a different chip |
| Display | `NativeLegacyDisplay.cpp` `sc7Display()` | AMS699VC01 only | Panics on any other panel; LCD path unreachable |
| Sleep | `SwitchSuspend` | BPMP-FW B01 owns SC7 entry | Erista enters SC7 through the SC7-Entry firmware; unreviewed |
| Model strings | `kernel/xnu-*/osfmk/arm64/cpu.c`, `native_oled_adt.py` | `nintendo,aula`, `NintendoSwitchOLED` | Wrong model, harmless to boot, wrong to ship |
| Audio, ALS, Wi-Fi/BT clock | `SwitchAudio`, `SwitchALS`, `SwitchPMIC` | MAX77620 LDO6 and 32 kHz clock writes, I2C2 | Same PMIC family, board wiring unchecked |

Read-only users (RTC read, button reads, MAX77620 SD0 read) are not listed.

## Design

### Board table

One table keyed by what U-Boot reports, used by the script and the loader:

| `t210b01` | `sku` | Board | Boots |
|---|---|---|---|
| 1 | 3 | OLED (Aula) | yes, as today |
| 0 | 0 | Erista (HAC-001) | only in the minimal mode, and only once its DTB and `native_gate` values are pinned |

- A row is accepted when `t210b01`, `sku` and the DTB's model string all agree. Any other combination keeps `WRONG_BOARD`.
- The Erista row ships disabled. Enabling it is a reviewed one-line change, made after the Console info check and the DTB.
- The loader gets the row's id and passes it on in boot-args, so `native_gate` picks the Erista carveout values from the
  same row instead of a second `if`.
- Per-version contents (kinds, pins) stay in the per-build tables, as the project rules require; the board table is not
  version specific.

### Minimal Erista mode

Goal: reach the console with the fewest chips touched. A profile id from the board table decides which services start, so
nothing relies on a driver noticing it is on the wrong board.

On:

- boot CPU only (`--//boot:smp` off), the loader, the SD exporter, the SD root and the hekate framebuffer
- RTC read, MAX77620 power-off and restart, button reads
- log and panic record to the SD card

Off, one profile check each at the place that starts it:

- GPU, video blocks, audio, Wi-Fi and Bluetooth, USB, USB-PD, touch, ALS
- BQ24193 writes and the charge policy
- BPMP EMC requests, DFLL and any CPU rail access
- system sleep

The charger is left as hekate and BL31 left it. The battery is not managed in this mode, so a test needs a charged battery
and a short run.

Not part of the mode and not decided: display. hekate's framebuffer is on screen at handoff; the new LCD power-up code only
matters after the first sleep.

## Done without the console

Committed on `erista-port`, not built with Bazel, not run on hardware.

1. Board table: `kits/SwitchBoard/SwitchBoard.h`. The boot script's check is generated from it, and the loader reads
   HIDREV and FUSE_RESERVED_ODM4 with the MMU off and refuses a row with `boot 0` (0x306). The Erista row has `boot 0` and
   no features.
2. Minimal mode: the loader appends `switch-board=<name>` after `bootargs.txt`. XNU's boot-arg parser keeps scanning, so the
   last one wins and the card's file cannot override it. `switch_board_has(feature)` gates every board driver in
   `SwitchRootBoot.c` and the clock, EMC and thermal calls in `switch_pmic_start`.
3. Model strings: the OLED row holds `Nintendo Switch (OLED model)` and `nintendo,aula`. The Erista row holds `NULL`.

How the kernel decides (`switch_board_resolve`): the registers (HIDREV, ODM4), the loader's `switch-board` word and the
device tree's `compatible` list must name the same row, or the board is unknown and has no features. Without a word only
the OLED is believed, and only where the registers say OLED. A missing, empty, cut off or duplicated word that disagrees,
registers that do not answer and a device tree that does not list the row all give no features. The Erista row has no
device tree strings, so it cannot be confirmed until they are read off its DTB.

The device tree check exists in the kernel only. The boot script and the loader decide from the registers; the loader
does not look at the DTB it copies. The OLED's strings are tied to `native_oled_adt.py` by a test.

`cpu.c` in both XNU trees now asks the table (`CACHE_IDLE`, by the device tree's compatible list alone, since it runs
before the registers can be read). The OLED row has the bit, the Erista does not.

Side effects checked for the mode: power-off is a MAX77620 soft reset and the RTC is read only. Panic records and logs go to
the SD card, not the eMMC. `switch_pmic_mark_start` writes the MAX77620 RTC alarm in DEVELOPMENT builds only.

Side effect of the stricter rule: a kernel run without the loader (the emulator) starts board drivers only if its
registers answer HIDREV and ODM4 as an OLED. The emulator's fuse page does (ODM4 type 4); its HIDREV is unconfirmed. Run
`bazel run //tools/emu:check` on the Mac for both kinds before relying on it.

### SE2 without SE2

`TegraSE` drives SE1 and SE2 as the kernel's AES and SHA engines. With the driver not started, `tegra_se_aes`, `tegra_se_cmac`
and `tegra_se_digest` return -1 (every algorithm is off until the driver turns it on), `NativeAES` then runs on the CPU, and corecrypto's
compress hook stays unset, so XNU hashes on the CPU. Both are the paths already taken when an engine declines. Nothing in
the SD root, the block driver or the root boot calls the engine. What changes is speed, and the kernel PRNG no longer gets
the engine's reseed on top of the loader's seed. The loader keeps the pinned seed when SE2 is off.

Other SE2 users, now gated by the SE2 bit: the loader's seed mix, `native_aks_se_probe` (DEVELOPMENT and DEBUG only) and the
board thread's `tegra_se_start`. Not found in the sources: whether the T210 has an SE2 at all. This stays open, and SE1 is
turned off with it, which is the safe direction.

Not run: any of this on a kernel. The reading is from the sources.

### Hardware touched on every board

These run for the Erista too, once its row is enabled. None of them is gated, and none is verified on a HAC-001:

- SD root: `NativeSwitchBlock` writes MAX77620 LDO2_CFG (0x27) on I2C5 0x3c to set the SDMMC1 supply (3.3 V, 1.8 V for UHS),
  and drives SDMMC1 and PLLP. The same PMIC family and the card wiring are assumed.
- `SwitchPMIC`: reads ONOFFCNFG1, publishes the RTC (read only), installs the MAX77620 soft reset as the power-off,
  maps PMC. The thermal record read is now behind THERMAL.
- `NativeSwitchWatchdog`: no hardware writes (WDT4 in debug kernels only).
- `TegraGPIO` eager start and `SwitchButtons` are behind BUTTONS (bit 16) and off for the Erista. GPIO still starts when a
  driver asks for a pin interrupt or output.
- MC error interrupt: SoC internal. The NVRAM controller was not read for this list.

The line by line audit is in `ERISTA_RESEARCH.md`.

Each feature bit needs its own review for the HAC-001 before it is set.

## Order of work

Order to the first XNU start, with the loader diagnostic first: `ERISTA_LOADER_DIAG.md`.

Without the console:

1. LCD `TegraPanel` and backlight; then lift the `sc7Display()` panel gate for the LCD only.

Needs the console, held back:

- Console info photo, `t210b01=0`/`sku=0` check, `native_gate` MC values, Erista SD0 and VDD_CORE, touch chip, PD controller,
  charger limits for the HAC-001 battery, the Erista model strings, and every boot test.

## Checks run without Bazel or hardware

Bazel cannot run here: `MODULE.bazel` registers a macOS arm64 execution platform only. The Mac build is still to do.

- `bazel/pinned/SHA256SUMS`: every pinned file matches.
- `tools/package/test/native_xnu_layout_test.py`: 22 tests pass. `boot_script_board_test.py`: 9 pass. `bl33_stage_test.py`
  needs `lz4`, and the other Python tests need Bazel runfiles.
- Board kit (13 tests) and loader boot-args (3 tests): run against a small GoogleTest stand-in, because GoogleTest and `lz4`
  cannot be installed here (no package network). The real GoogleTest run is still to do on the Mac.
- `SwitchRootBoot.c` and `SwitchBoard.h`: clang `-Wall -Wextra -Werror` syntax checks with shim headers.
- `TegraDisplayPower.cpp`: `g++ -std=c++20 -fsyntax-only -Wall -Wextra` with stub headers, no warnings.
- JDI panel table: 43 of 43 entries equal hekate `di.inl`.
