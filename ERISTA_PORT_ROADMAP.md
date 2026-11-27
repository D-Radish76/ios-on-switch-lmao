# Erista: roadmap from the first XNU entry to touch on SpringBoard

Written 2026-11-27 from the source reading in `ERISTA_XNU_BOOT_STAGES.md` and `ERISTA_IOKIT_DEPENDENCIES.md`. It orders work; it approves nothing. Every phase that touches the console needs the owner's written approval for that run. `boot` stays 0 and the feature mask 0 until a person changes them, the first XNU run is on HOLD, T3 and T4 are not waived. Marks: **confirmed**, **inferred**, **unknown**. No dates or durations are given because none was measured.

How this relates to the older plans: `ERISTA_NATIVE_PORT_PLAN.md` lists the blockers per row and `ERISTA_FIRST_XNU_BLOCKERS.md` the steps to the first run. This file starts where those end and adds the later phases.

## Where the work stands

| Done and measured on the Erista | Built or designed, not run | Open |
| --- | --- | --- |
| EL1 state, DRAM banks, MC words, GIC reads, `PSCI SYSTEM_RESET` returning to hekate (D0, D0-R, seven runs), TMP451 ID and one-shot (T1, T2), SDMMC1 clock and reset (D0-S v2), sector 0 read (D1-min), T4-R reads of the TKE watchdog block (all zero) | trace kernel, stop-after-VM reset, Erista ADT (one core), `boot` gate option B, T4-A0 host model of the watchdog gate | T3 and T4 (built or waived), the seed decision, the run condition, `boot` 1 for a test build, every XNU run |

## Phases

### P0. Close the holds before any XNU instruction runs on the console

| Item | Basis | State |
| --- | --- | --- |
| T3 (temperature display) and T4 (hang protection) built and proved, or waived in writing | `ERISTA_FIRST_XNU_BLOCKERS.md` step 7 | open. T4-A reached host design only; where the console lands after a watchdog reset is unknown (`ERISTA_T4A_RESET_PATH.md`) |
| Seed decision | blockers step 6 | the owner prefers a per-install seed; the seeded package that can boot does not exist |
| Build flag giving `boot` 1 to the test build only | `ERISTA_FIRST_XNU_BOOT_GATE.md` | implemented offline, flag not approved |
| Run condition (how long the stop rules allow) | blockers step 9 | held by the owner |
| Mac verification of one commit | blockers step 1 | partly done |

Exit: the owner approves one attended run in writing.

### P1. First XNU entry: stages 1 to 21 and the timed stop

- Run: trace kernel, one core, `switch_stop_after_vm`, a person at the console, hand on POWER (`ERISTA_FIRST_XNU_RUN.md`).
- Expected on screen: marks 1 to 21, then `XNU VM INITIALIZED`, then a timed `PSCI SYSTEM_RESET` back to hekate.
- What it settles: stages 1 to 6 on an A57 with the Apple page-table setup (**unknown** today), the framebuffer identity map, `early_random` with the repeating seed, `arm_vm_init`.
- Failure reading: LAST STAGE names the stage; stages 7 to 15 have almost no MMIO, so a fault there points at memory or the ADT (`ERISTA_XNU_BOOT_STAGES.md`).
- Exit: one clean run recorded, then a second to rule out luck. Nothing later starts before this holds.

### P2. Past VM init without a root file system

- Remove `switch_stop_after_vm`; keep the card without a root partition, so the kernel runs `PE_init_platform(TRUE)`, `PE_init_iokit`, `bsd_init`, IOKit matching and waits 30 s for the root (`IOKitBSDInit.cpp:795`), then panics `Failed to mount root device`.
- What it settles: GIC bring-up and the first interrupt on the Erista (**unknown**; the generic `pe_arm_map_interrupt_controller` finds no node and is skipped, `ERISTA_XNU_EARLY_INIT.md`), the generic timer under XNU's `cntkctl_el1` (a `wfe` wake has not been shown, `ERISTA_FIRST_XNU_BLOCKERS.md`), IOKit root, `ArmDTPlatform` and `ArmCpu` matching, the watchdog driver with no hardware.
- Expected stop: the panic screen, a person at POWER. This phase has the first interrupt-dependent code, so a hang is more likely than in P1 and nothing resets it unless T4 exists.
- Exit: the panic text is the expected root-wait panic, nothing earlier.

### P3. SD driver and root mount

- Prerequisites: an Erista card with one APFS partition the loader's SD reference accepts (rows 16 and 18 of `ERISTA_NATIVE_PORT_PLAN.md` measured or removed with a reason); an SD tuning decision (`switch-no-sd-uhs` for the first run); a decision about the CMD49 pair, which cannot be switched off without a driver change.
- Run: the SD driver reads the GPT and publishes after IOBSD (`switch_boot_autoconf_done`), `vfs_mountroot` mounts it read-only first if the code allows (**unknown**; the card gate refuses writes outside the APFS partition and panics otherwise).
- What it settles: XNU's SDMMC1 driver on a T210, the DMA path with `TegraSMMU` (**unknown**), APFS on the card.
- Risk: the card is written by the OS from this phase on. The card image is backed up before each run, the hash is compared after.
- Exit: root mounted, then a panic or a stop just after `switch_boot_mountroot_done`.

### P4. launchd and a userspace that stays alive

- Prerequisites: P3. `vm_init_before_launchd`, `load_init_program` start `launchd` (**inferred** path).
- Needs, in the order the code starts them: `NativeSwitchWatchdog` before launchd reads its LaunchEvents; NVRAM controller (`switch_nvram_publish`); the board thread with mask 0.
- What it settles: the first user process, the Mach and BSD side, the Data volume mount. SpringBoard cannot start without more (P6).
- Failure reading: serial is absent; the screen's panic text and the saved panic record (`SwitchPanicSave`) are the only evidence.

### P5. Controls and thermal first, then the rest (feature bits one at a time)

Order from `ERISTA_NATIVE_PORT_PLAN.md`: buttons (only way to wake the screen), read-only temperature display, then THERMAL. Each is one reviewed feature bit with a host test and an attended run.

| Bit | Needs from the Erista | Open question |
| --- | --- | --- |
| BUTTONS | GPIO, MAX77620 IRQ masks | MAX77620 is common; interrupt delivery unmeasured |
| THERMAL | TMP451 offset 0 (not -12.5), Erista fan and 5 V rules, DFLL and the CPU rail (MAX77621), SOC_THERM trips | the driver carries Mariko values; hardware protection against heat is not shown |
| PMIC | MAX77621 rail support | `SwitchPMIC` knows MAX77812 only |
| CHARGER | BQ24193, MAX17050 (check against the Erista) | not compared |

SMP (PSCI `CPU_ON` for cores 1 to 3) belongs here too, after interrupts work; the first run for it is its own attended step.

### P6. Display and SpringBoard

- Display: first adopt hekate's lit panel; later add the InnoLux panel init, the PWM0 backlight back end, and widen the `sc7Display` gate (`ERISTA_RESEARCH.md` sections 1 and 10).
- GPU (`SWITCH_FEATURE_GPU`): GM20B clock and rail setup for the Erista (the T210B01-only checks in `TegraDFLL/platform.c:695` and the rails in `gpu_power.c:187` are the places that differ). Whether SpringBoard can draw without the GPU in this tree is **unknown**; the Apple graphics stack is not started with the bit off. A crash in Apple code at this point means a contract is missing on our side (`CLAUDE.md`); fix it at the source.
- Exit: SpringBoard on screen, then a stable minute.

### P7. Touch

- `SwitchTouch` is an FTM4-family driver; the Erista's touch controller, its bus and its interrupt are **unknown** here and must be identified first (the OLED's description in the ADT must not be reused). Then the HID path (`NativeHidInput`) and a calibration against the 720x1280 panel.
- Exit: a tap on an icon opens an app.

### P8. Later features

USB, Wi-Fi, Bluetooth, Joy-Con, audio, sensors, sleep. Each depends on a part that was not compared with the Erista (`ERISTA_IOKIT_DEPENDENCIES.md`); each is a separate bit and a separate attended run.

## Version containment

Every phase changes one tree at a time. iOS 17 (xnu-10063, `--config=ios17`) and iOS 27 (xnu-12377) differ by build tables and trees; a fix for one must not change the other, and each change is checked on both (`CLAUDE.md`). The stage and path lines in this set of documents are iOS 17 only.

## What cannot be planned yet

- Where the console lands after a watchdog reset, so T4's recovery story.
- Whether the A57's clock and voltage at the handoff are safe for minutes of XNU.
- The Erista's touch controller and which parts of the wireless, audio and charger sets differ.
- Whether anything in P2 to P4 needs the BPMP, which BL31 powers off.

## Sources

`ERISTA_XNU_BOOT_STAGES.md`, `ERISTA_IOKIT_DEPENDENCIES.md`, `ERISTA_NATIVE_PORT_PLAN.md`, `ERISTA_FIRST_XNU_BLOCKERS.md`, `ERISTA_FIRST_XNU_BOOT.md`, `ERISTA_FIRST_XNU_RUN.md`, `ERISTA_ADT.md`, `ERISTA_RESEARCH.md`, `ERISTA_THERMAL.md`, `ERISTA_T4A_DESIGN.md`, `ERISTA_T4A_RESET_PATH.md`.
