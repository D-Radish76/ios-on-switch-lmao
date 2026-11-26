# Erista native port plan

Survey of the tree at this commit. Nothing here changes code or lifts `boot 0`. "Measured" means read on the Erista by
D0, D0-R, D1 or T1 v3. "Unverified" means no Erista reading exists and the item must not be judged safe.

## A-1 Boot blockers

Everything between a Nintendo Switch (2017) and the first XNU instruction, in the order the code reaches it.

| # | Where | Current behaviour | Needed for the Erista | Measured data |
| --- | --- | --- | --- | --- |
| 1 | `kits/SwitchBoard/SwitchBoard.h:47` (`SWITCH_BOARDS`) | `X(ERISTA, "erista", 0, 0, 0, 0, ...)`: `boot` is 0 and the feature mask is 0 | Set `boot` to 1 only by a person, after every row below is closed. Features are added one reviewed bit at a time | HIDREV `00022117`, ODM4 `00000204` identify the row |
| 2 | `tools/package/boot_script.py:34` (`board_gate`) | Emits `READY` only for rows with `boot 1`, otherwise `WRONG_BOARD` | None. It follows the table | none |
| 3 | `boot/u-boot-script.txt:19` (`@BOARD_GATE@`) | Refuses the handoff on `WRONG_BOARD` | None | none |
| 4 | `boot/u-boot-script.txt:43` | Requires `0xF5A00000`, 720x1280, `a8b8g8r8`, rot 3 | Nothing if the properties match | Framebuffer address is hekate's. The console drew on the Erista, but the properties were not read (open) |
| 5 | `boot/u-boot-script.txt:109` | `fdt rsvmem add 0xF5A00000 0x400000` | None | Same address as row 4 |
| 6 | `boot/u-boot-script.txt:142-205` | `bootm_low 0x80000000`, `bootm_size 0x7FD00000`, `switch_native_xnu=1`, then `bootm go` | None if bank 0 matches | Bank 0 `0x80000000` + `0x7FD00000` |
| 7 | `boot/loader/loader.c:288` (`loader_after_transition`) | Refuse `0x301` unless EL1h, SCTLR clear, DAIF masked, CNTFRQ 19.2 MHz, HCR/CNTHCTL/CNTVOFF as the EL2 drop leaves them | None expected | CNTFRQ 19.2 MHz measured. EL2 drop result on the Erista: only D0-R's register log, check before closing |
| 8 | `boot/loader/loader.c:292-294` | Reads HIDREV and ODM4, refuses `0x306` when `board->boot` is 0 | Same as row 1. This is the second place `boot 0` stops the loader | measured |
| 9 | `boot/loader/loader.c:370` (`native_gate` call) | Any non-zero return refuses | See rows 10 to 13 | |
| 10 | `boot/loader/loader.c` (`native_gate`), `boot/loader/carveout.h` (`switch_carveout_table`) | Done (N1): the OLED name test is gone. A board with no table is refused `0x402` before anything is read | none | The 25 values equal the OLED's |
| 11 | `boot/loader/loader.c:253` | Entry SP in `[0xf0001000, 0xffd00000)` else `0x401` | None | `FF87D010` and `FF87DD10` pass |
| 12 | `boot/loader/carveout.h` (`switch_carveout_check`) | Done (N1): one table per board, `switch_carveout_oled` and `switch_carveout_erista`, refuse `0x410 + i`. Host test `carveout_test` | none | All 25 read equal. Carveouts: GPU FW `0xFFD00000`, GPU WPR `0xFFD40000`, secure FW `0xFFE00000`, TZDRAM `0xFFF00000` |
| 13 | `boot/loader/loader.c:270-274` | Reads six GIC-400 registers into the probe record, no refusal | None | GIC-400 at `0x50041000` and `0x50042000` read on the Erista |
| 14 | `boot/loader/loader.c:380-384` | CRC32 field of `ID_AA64ISAR0_EL1` must be 1, else `0x305` | None if the A57 reports 1 | Not recorded in the notes I read: confirm in the D0-R log |
| 15 | `boot/loader/loader.c:406`, `:454`, `:460` | Kernel CRC (`0x302`, `0x303`) and boot-args shape (`0x304`) | None, board independent | none needed |
| 16 | `boot/loader/loader.c:418-422`, `:516-525` | SD reference validation, refuse `0x640+` and `0x650`, `0x660+` | Needs the Erista SDMMC1 path to match (row A-3 SDMMC) | Not measured |
| 17 | `boot/loader/memory.c:205-214` | Control FDT `/memory` must be exactly `0x80000000+0x7FD00000` and `0x100000000+0x80000000`, else codes 25 and 26. `bank_count = 2` | None | Both banks measured |
| 18 | `boot/loader/loader.c:485-491` | Refuse `0x710` if the SD engine at `0x700b0024`/`0x700b002f` is busy or `bank_count != 2` | None | Not measured on the Erista |
| 19 | `boot/loader/loader.c:537` | SE2 seed mixing only if the board has `SWITCH_FEATURE_SE2` | None: the Erista row has no SE2 and keeps the pinned seed | The Erista DTB has one `se` node |
| 20 | `boot/loader/loader.c:544` | `loader_enter_kernel(LOAD_ARGS, LOAD_ENTRY)` | None | |
| 21 | `tools/package/native_oled_adt.py:186` (`generate_adt`) | Only the root identity and SoC naming follow the board. Everything else is the OLED's tree | An Erista ADT (panel, touch, PMIC set, SDMMC tuning, no SE2). Its own docstring says the tree is not usable until audited | DTB comparison in A-3 |
| 22 | `diagnostics/switch/SwitchRootBoot.c:55-84` | Drivers start per feature bit. The Erista mask is 0, so the minimal set starts | Add bits only after the matching driver row in A-3 is closed | |
| 23 | `boot/BUILD.bazel:28-33` (`SMP = True`) | Default build is four cores | `--//boot:smp=false` for the first boot (see A-2) | |
| 24 | `native_gate` is compiled under `LOAD_NATIVE_GATES` (`loader.c:246`) | Release and normal builds define it. The diagnostics do not use this loader | None | |

Problems found while surveying (reported, not changed)
- Row 10 is the only place where the code names a board instead of reading the table. Anything else added later
  should go through the table.
- Row 21 means a build for the Erista today would carry an OLED panel and touch description. `boot 1` must not be set
  before the ADT is board specific.
- `ERISTA_LOADER_DIAG.md:44` still lists the framebuffer properties as open (row 4).
- Rows 7, 14 and 18 have no Erista reading in the documents I read. They are unverified, not safe.

Conditions to lift `boot 0` (none of these is done)
1. Rows 10 and 12: done in N1 (separate tables, host tests).
2. Rows 4, 7, 14, 16, 18 each have an Erista measurement or are removed from the Erista path with a reason.
3. An Erista ADT exists and its differences from the OLED tree are audited (A-3).
4. A person sets `boot 1`. Then the Erista build is also checked on both iOS versions (CLAUDE.md, version containment).

## A-2 Memory and CPU

### Memory
- DRAM: bank 0 `0x80000000` + `0x7FD00000`, bank 1 `0x100000000` + `0x80000000`. `memory.c:205` already accepts exactly
  these. Measured on the Erista. DRAM stays at 204 MHz (no EMC raise), so the EMC feature stays off.
- Carveouts: the four values in row 12 equal the OLED's. The kernel layout (`tools/package/native_xnu_layout.py`) is
  derived from these and the loader's destination, boot-args page and stamp slots, which D0 notes call board
  independent. No Erista specific layout change is needed on the evidence so far.
- Reservations: one DTB entry `0x80000000..0x80020000`, one added by the script for the framebuffer
  `0xF5A00000..0xF5E00000`. The `0xA0000000..0xA0020000` ledger entry (`loader.c:493`) has no Erista reading
  (unverified).
- MC error reporting: `drivers/TegraSMMU/TegraMCErrors.cpp:158` reads MC registers. Same SoC family. Unverified on the
  Erista.

### CPU and handoff
- Cores: four Cortex-A57 in both DTBs. `SWITCH_FEATURE_CACHE_IDLE` is A57 only and the Erista is an A57, but the feature
  is off in the Erista row.
- Init path: U-Boot at EL2, then `loader.c` drops to EL1 (row 7), then `boot/handoff/entry.S`, then
  `kernel/xnu-12377.121.6/osfmk/arm64/start.s` (`:103-144` reset vector, `:195-215` MMU and cache enable,
  `:329-423` boot CPU path). xnu-10063 has its own tree. MMU, caches and the vector table are set by XNU's `start.s`
  from the boot args, with no board specific code in that path that I found.
- Secondary cores: `osfmk/arm64/cpu.c:120-153` and `switch_smp.h` start them over PSCI through `drivers/ArmCpu`
  (`ArmCpu.cpp:83-90`, `:762-770`). BL31 is the Erista's own BL31 (same hash), and BPMP is powered off by it on T210.
  PSCI `CPU_ON` for cores 1 to 3 has not been run on the Erista (unverified). PSCI `SYSTEM_RESET` from EL1 works.
- GIC and timer: GIC-400 and the 19.2 MHz architected timer. `ArmGIC.cpp` and the ADT carry the same addresses as the
  Erista DTB. Read only on the Erista; no interrupt has been taken (unverified).
- Single core first boot: feasible by design. `--//boot:smp=false` selects the one-CPU ADT, so no PSCI `CPU_ON` is
  needed. Whether the single-core build links and boots unchanged is unverified, since the default and the release
  are both four cores. It is the recommended first step because it removes one unmeasured firmware call.
- Clock and voltage: XNU does no DVFS, so the A57 stays at the rate hekate and U-Boot left. The Erista value is not
  measured (`ERISTA_RESEARCH.md` section 11). First boots stay short and attended, and the temperature read-only
  display (T2) comes before THERMAL.

## A-3 ADT and drivers

Erista DTB `bazel/pinned/firmware/erista-sku0.dtb` (974 nodes) against the OLED DTB `oled-sku3.dtb` (1042 nodes) and
the Apple tree from `native_oled_adt.py`. Classes: Required (boot stops without it), Replaceable (needs an Erista
version), Disableable (feature bit stays 0 at first boot).

| Block | Erista vs OLED | Driver or code | Class at first boot | Notes |
| --- | --- | --- | --- | --- |
| CPU | Both four A57 | `ArmCpu` | Required | One core first (A-2) |
| Interrupt controller | Both GIC-400, same bases | `ArmGIC` | Required | Read only so far |
| Timer | Both armv8 timer 19.2 MHz | XNU generic timer | Required | CNTFRQ measured |
| Memory controller | Both MC/EMC/SMMU nodes. 25 MC values equal | `TegraSMMU`, MC error code | Required | EMC scaling stays off |
| SDMMC | Same controllers. Erista tap delay 4, trim 2. OLED 0xb, 0xe | `NativeSwitchBlock.cpp:3715` | Replaceable | Root volume is on SDMMC1. Erista tuning values come from its DTB. `switch-no-sd-uhs` suggested for the first boot. Unverified on the Erista |
| PMIC | Erista: max77621 at 0x1b and 0x1c plus max77620 at 0x3c. OLED: max77812 at 0x33, max77620 at 0x3c, max77801 at 0x18 | `SwitchPMIC.cpp:774-803` | Replaceable | max77620 (RTC, power off, reset) is common and required. CPU and GPU rail drivers differ. No rail change at first boot |
| Display | Erista panel InnoLux P062CCA-AZ1, OLED panel differs | `NativeLegacyDisplay.cpp:342`, `:5344-5351`, `TegraDisplayPower.cpp:483`, `:581` | Replaceable | First boot adopts the panel hekate lit (framebuffer `0xF5A00000`). The panel init in the tree is OLED shaped and untested on the Erista |
| GPIO | Same SoC | `TegraGPIO` | Required | Buttons (`SWITCH_FEATURE_BUTTONS`) are the first feature to add, since they are the only way to wake the screen |
| USB | Both XUSB. Charger and PD parts differ by board | `SwitchUSB`, `SwitchPD`, `SwitchBattery` | Disableable | Charger parts (`BQ24193`, `MAX17050`) not checked against the Erista |
| GPU | GM20B both. T210 versus T210B01 clock and rail differences | `TegraGM20B` (`gpu_clock.c:131`, `gpu_power.c:187`), `TegraDFLL/platform.c` | Disableable | GPU rail is the max77621 on the Erista. Apple graphics stack is not started without the bit. A crash in Apple code when it is later enabled is a missing contract on our side |
| Audio | Codec wiring differs | `SwitchAudio` | Disableable | Unverified |
| Touch | Different controller | `SwitchTouch` | Disableable | The OLED description in the ADT must go from the Erista tree |
| Wi-Fi / Bluetooth | Module and clock source not compared | `SwitchWLAN`, `SwitchBluetooth` | Disableable | Needs the MAX77620 32 kHz clock. Unverified |
| Thermal | TMP451 at I2C1 0x4C (T1 v3 read ID 55). Both have SOC_THERM and pwm-fan | `SwitchThermal.cpp` | Disableable, then first to add | T2 gives a read only temperature. THERMAL also drives DFLL and the CPU rail, which differ |
| Security engine | Erista: one `se@70012000`. OLED adds a second engine (SE2) | `TegraSE`, `loader.c:537`, `AKSDeviceKey.cpp:155` | Required (SE), SE2 absent | The kernel uses SE for keys. SE2 stays off for good |
| Suspend and RTC mark | Board independent code, uses MAX77620 | `SwitchSuspend` | Disableable | |
| Fan | pwm-fan both | `SwitchThermal.cpp` | Disableable | Without THERMAL nothing controls the fan after hekate's `fan_set_duty(0)` |

## Suggested order (not started)

1. Per-board carveout table in `native_gate` with host tests (rows 10, 12). Done (N1).
2. Close the open measurement rows (4, 7, 14, 18) through read-only diagnostics, one at a time, each approved.
3. Erista ADT generator path: panel, touch removed, SDMMC tuning from its DTB, one CPU.
4. Single-core build, still `boot 0`, checked on both iOS versions by build and emulator.
5. A person decides `boot 1` for one attended run. Buttons next, then T2 temperature display, then THERMAL.

## Not measured on the Erista

EL2 drop and `ID_AA64ISAR0` readings (rows 7, 14), SD engine idle state (18), SD reference path (16), framebuffer
properties (4), PSCI `CPU_ON`, interrupt delivery, CPU clock and voltage at handoff, every driver in A-3 other than the
read only ones noted.
