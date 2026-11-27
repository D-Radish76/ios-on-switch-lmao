# Erista: what remains before the first XNU run

Written 2026-11-27. It sorts the open T3 and T4 work by where its answer can come from and orders it by dependency. It approves nothing and changes no code. `boot` stays 0, the feature mask 0, T3 and T4 are not waived, T4-A1, T4-A2 and T4-P are not implemented, no watchdog is armed, the first XNU run is on HOLD. Marks: **confirmed** (read in the file or at the URL named), **inferred**, **unknown**.

No Switchroot Linux card is at hand, so nothing below was taken from a running Linux. Web sources were read through a fetch tool that summarises, so quotes are close and not byte-checked.

## Where it stands

| Done | Open |
| --- | --- |
| Loader at EL1, DRAM, SDMMC1 clock, sector 0, TMP451 one-shot (D0, D0-R, D0-S v2, D1-min, T1, T2), T4-R reads of the TKE block (all zero), `PSCI SYSTEM_RESET` returning to hekate (seven runs), stage 1 to 21 reading, built kernel `TCR_EL1` (4 KiB, static) | T3 and T4 (built or waived), the seed decision, the run condition, `boot` 1 for a test build, every XNU run |

## A. Answerable from existing source, no hardware

| ID | Item | State | Source |
| --- | --- | --- | --- |
| A1 | Which watchdog drivers Switchroot's kernel builds | **answered this round.** The `linux-dev` defconfig of `CTCaer/switch-l4t-kernel-4.9` has `CONFIG_WATCHDOG`, `MAX77620_WATCHDOG`, `TEGRA21X_WATCHDOG` and `SOFT_PLATFORM_WATCHDOG` set to y, and `TEGRA_SOCTHERM`, `NV_TEGRA_BPMP`, `MAX77620_THERMAL`, `TEGRA_THERMAL_THROTTLE` too. No `CONFIG_TEGRA_WATCHDOG` line appears, so the in-tree `tegra_wdt.c` is **inferred** not built. That closes item 5 of `ERISTA_T4A_DESIGN.md` (mainline's driver does not bind on this kernel, inferred) | `arch/arm64/configs/tegra_linux_defconfig` |
| A2 | The `tegra21x` watchdog driver: write order, meaning of `nvidia,timer-index` 7, expiry handling (T4-A items 4, 6, 8) | **open, narrowed.** The config option exists, but `drivers/watchdog/Kconfig` of the 4.9 tree has only `TEGRA_WATCHDOG` and `TEGRA_WATCHDOG_LEGACY`, so the driver is in another tree. A guessed path in the NVIDIA-side repository did not fetch. A forum post shows the module name `tegra_wdt_t21x`, the device `60005100.watchdog` on a Jetson and a kernel thread that kicks it. None of that gives register detail | the repository that holds the driver is **unknown**; NVIDIA's L4T kernel source would |
| A3 | TRM tables: index to PTV address, PMC2CAR scope, TKE survival over a reset, `WDT_DURING_BR` (T4-A items 4, 9, 10, 11) | open. The TRM is not in the repository. If the owner holds a copy, these four rows are a reading task | the TRM |
| A4 | Whether the MAX77620 watchdog stays enabled across the restart it causes | open, not read. It matters only if the PMIC option of `ERISTA_T3T4_MINIMAL.md` is revived. Switchroot builds `MAX77620_WATCHDOG`, so the Linux driver and the part's datasheet can be read | `max77620_wdt.c`, datasheet |
| A5 | hekate bit values behind "FIQ only" | open. `soc/timer.h` at v6.5.5 was not read; "no reset bit" rests on the mode name. Also untraced: which of Nyx and the bootloader runs `hw_deinit` last on a Nyx launch | hekate v6.5.5 |
| A6 | TF-A: PSCI `SYSTEM_RESET` and any TKE watchdog code on T210 | not read. The pinned BL31 is Nintendo's binary, already scanned (one TKE constant, `TMRUS`). Upstream source can suggest, not prove | TF-A `plat/nvidia/tegra/soc/t210` |
| A7 | XNU side: Apple instructions outside `start.s`, macro-generated STARTUP entries, `ml_init_timebase` when skipped | open, readable in the tree (`ERISTA_XNU_EARLY_INIT.md`). They bear on stage 1 to 21, not on T3 or T4 | `kernel/xnu-10063.141.1` |

## B. Needs a running Linux on the Erista

Not possible now. Each row is a read on a Linux the owner starts, not an image of ours.

| ID | Item | What it would give | Limit |
| --- | --- | --- | --- |
| B1 | Fan: rpm range and edge rate under `pwm-fan` | an expectation for T3's polled tachometer count, which loses edges when fast | Linux's own PWM and 5 V setup, not hekate's handoff state |
| B2 | Thermal zones, trip points, TMP451 and SOC_THERM readings, idle and under load | a basis to replace the provisional 40, 50 and 55 C figures | Linux calibrates the sensors with its driver; T3, T4 and the loader have no such calibration |
| B3 | Whether Linux uses a watchdog at all (`/dev/watchdog*`, dmesg) | the pinned DTB has `watchdog@60005100` disabled, so probably none | an expiry test would be a watchdog arming: not proposed here, needs the owner's separate approval |
| B4 | MAX77620 registers (`CNFGGLBL2`, `CNFGGLBL3`, `ONOFFCNFG`) and its thermal shutdown threshold, by I2C reads | the part's state, not the handoff state | none of the loader's I2C5 work exists |

What Linux cannot answer even with the card: the state hekate and Nyx leave at the handoff (PWM clock, pinmux, the 5 V rail, the A57's clock and voltage), because Linux rewrites it. That is group C.

## C. Only our own diagnostics settle it

| ID | Item | How | State |
| --- | --- | --- | --- |
| C1 | State at the handoff: `PWM_CSR_1`, pinmux, GPIO, the 5 V rail, `MPIDR_EL1`, `ID_AA64MMFR0_EL1.PARange`, `CPUECTLR_EL1` as XNU sees them | a read-only loader step; MMIO reads are new, CPU registers are not | not approved. MIDR and TGran4 were measured; `MPIDR` and `PARange` were not found in the documents searched |
| C2 | `wfe` waking under XNU's `CNTKCTL_EL1`, and `SYSTEM_RESET` after XNU owns the core | the VM-stop run itself | proven from the loader only |
| C3 | XNU's early heat and the time between marks | the same run with video and an external temperature | unmeasured. D1-min's one observation was 4.1 C over hekate, Nyx and a 15 s dwell |
| C4 | EL1 writes to the TKE block, the period unit, the timer source by measurement, the state after a reset, where the console lands | T4-A2 only | forbidden now; T4-R zeros say nothing about which timer the watchdog counts |
| C5 | Fan edges and the 5 V rail write | T3 | not built |
| C6 | Time from reset to hekate's menu after XNU | the VM-stop run | no run has measured it |

## D. Safety around the first VM-done stop

| # | Measure | State |
| --- | --- | --- |
| 1 | A person at the console, a hand on POWER | the rule of `ERISTA_FIRST_XNU_RUN.md`; the owner's written acceptance is open |
| 2 | Timed `PSCI SYSTEM_RESET` after the VM text | built offline; **confirmed** from the loader in seven runs; unproven after XNU |
| 3 | Stop rules: 30 s with no new mark, `n + 10` s after the text | provisional; marks that trickle in allow about 10.5 minutes; the run condition is held (blockers step 9) |
| 4 | Start temperature below the provisional 40 C, read in Nyx | a person's rule, not a trigger |
| 5 | Recovery rehearsal without any image of ours: POWER hold from a running hekate, RCM jig from off, injection time | owner; recommended, because POWER hold is the only recovery of the run and is unverified for a hung XNU |
| 6 | Card photographed and hashed before and after; no root partition, so XNU reaches no SD driver | procedure exists (`ERISTA_FIRST_XNU_RUN.md`) |
| 7 | `boot` 1 for the test build only; seed decision; Mac verification of the whole set | all open (blockers steps 1, 6, 8) |
| 8 | Video with a clock, marks timed | in the run sheet |

Not covered by any measure: a hang before the VM text has no autonomous stop, the fan is off, and no layer reads a temperature. A watchdog would cover the hang only if its landing and recovery were known; they are not (`ERISTA_T4A_RESET_PATH.md`), so T4-A would add a stop whose recovery is unverified. That is why T4-A does not replace the person.

## Dependencies

- A2 and A3 gate T4-A1 (conditions 2 and 3 of `ERISTA_T4A_DESIGN.md`), unless the owner accepts a provisional timer source in writing.
- T4-A1 gates the T4-A2 prerequisites: run sheet, recovery rehearsal, separate approvals.
- T4-A2's period needs video timings. The design says they come from "the first run". If that means the first XNU run, T4-A cannot come before it; the owner should say which run is meant.
- T3 needs its read-only state step (C1) first, then the fan run.
- B and A4 to A7 gate nothing in the first run.
- D5 depends on nothing and can happen now.
- Every run needs the owner's decisions in D7 and a Mac check of one commit.

## Shortest order

**Without T3 and T4 (option C, a written waiver).** The owner chooses the waiver, the run condition and the seed. In parallel: Mac verification of one commit, the `boot` flag decision, the recovery rehearsal. Then one approved attended run. No further loader diagnostic is needed by any finding above. This path leaves every limit in the last paragraph of D in place.

**With T4-A (the owner does not waive).**

1. A2 and A3, or a written provisional source.
2. T4-A conditions 1 to 6; build T4-A1 and audit it on the Mac.
3. Recovery rehearsal, run sheet and the separate approvals.
4. T4-A2 run, then the next only if the landing is known.
5. Then the XNU run, with the watchdog carried into its loader only if the owner decides so.

Parallel to 1 to 4: C1, then T3 if the fan is wanted.

Which path is taken is the owner's decision. Nothing here lowers a HOLD.

## Sources

`ERISTA_FIRST_XNU_BLOCKERS.md`, `ERISTA_T3T4_OPTIONS.md`, `ERISTA_T3T4_MINIMAL.md`, `ERISTA_T3T4_REUSE.md`, `ERISTA_T4A_DESIGN.md`, `ERISTA_T4A_RESET_PATH.md`, `ERISTA_PROTECTION.md`, `ERISTA_THERMAL.md`, `ERISTA_FIRST_XNU_RUN.md`, `ERISTA_XNU_EARLY_INIT.md`; the Switchroot defconfig and `drivers/watchdog` Kconfig and Makefile at `linux-dev` of `CTCaer/switch-l4t-kernel-4.9`; the NVIDIA forum thread "tegra21x watchdog module".
