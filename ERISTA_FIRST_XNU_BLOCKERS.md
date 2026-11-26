# Blockers before the first XNU boot, after T2

State at 2026-11-26, after the T2 run (`ERISTA_T2_RESULT.md`); it replaces the list written after D1-min. Nothing here approves a run. `boot` is 0 and the feature mask is 0 for the Erista; T2, T3, T4, the full D1 and every XNU run are on HOLD.

## What the diagnostics have settled

| Run | Settled | Not settled |
| --- | --- | --- |
| D0, D0-R | EL1 state, MC rows, FDT walk, reserve map; PSCI `SYSTEM_RESET` returns to hekate | |
| T1-T, T1 v3 | I2C1 at divisor `00FF0001` reaches the TMP451; ID `55` | any sensor write, any temperature |
| D0-S v2 | SDMMC1 clock on and reset off at EL1; Present State and Clock Control readable (`ERISTA_D0S_V2_RESULT.md`) | D0-S v1's hang |
| D1-min | U-Boot's SD driver reads sector 0 into DRAM; the bytes equal the Mac's; the loader handoff after it works (`ERISTA_D1MIN_RESULT.md`) | XNU's SD driver; the exporter (D1) |
| T2 | one TMP451 one-shot at `0x4C`: BUSY seen, both temperature pairs changed, configuration `C0` after; 160 writes and 35 transfers as the host model gives; PSCI `SYSTEM_RESET` returned (`ERISTA_T2_RESULT.md`) | temperature or fan while XNU runs; any hang protection; a thermal limit |

## The steps to the first XNU boot, now

Replaces the table in `ERISTA_FIRST_XNU_BOOT.md`.

| # | Step | State | Blocks the run? |
| --- | --- | --- | --- |
| 1 | The Mac verifies everything on one commit (tests, builds, the trace-kernel scan, the loader hashes) | open until the Mac reports on the latest commit | yes |
| 2 | D0-S | done: v2 PASS | no |
| 3 | D1-min | done: PASS | no |
| 4 | T2 | done: PASS | no |
| 5 | A stop path | the person, plus the timed `SYSTEM_RESET` after VM done (built offline, not run). Proven from the loader at EL1 in seven runs (D0, D0-R, T1-T, T1 v3, D0-S v2, D1-min, T2); not proven after XNU owns the core | yes: the owner accepts the person in writing, or approves the run that tries the reset |
| 6 | Seed decision | the owner prefers the per-install seed (2026-11-26); the written decision of the run is still open | yes |
| 7 | T3 and T4 built and proved, or waived | open. Neither is built; the owner has not decided a waiver. See below | only if not waived |
| 8 | `boot` 1 for the build under test only: option B (derived board table, off by default) is implemented offline (`ERISTA_FIRST_XNU_BOOT_GATE.md`); the build flag that turns it on is not approved | open | yes |
| 9 | The run: trace kernel, one core, stop after VM, stop rules, a person at the console (`ERISTA_FIRST_XNU_RUN.md`) | not approved | |

Owner's decision (2026-11-26): the full D1 is not a condition of the first VM-stop diagnostic. The Mac's checks for the first-XNU package are in `ERISTA_FIRST_XNU_MAC.md`.

Two things this run list does not need: D1-min's result (the first XNU diagnostic ends before IOKit and the SD driver, so XNU never touches the card in it) and
the full D1.

## What D1-min does and does not buy

D1-min removes one worry about the loader's memory plan: a card read by U-Boot at `0xA001F200` leaves the loader's checks clean and hekate reachable afterwards.
It says nothing about XNU's SD path. `NativeSwitchBlock.cpp`, XNU's SDMMC1 setup, its DMA and the CMD49 pair stay unverified, and no step above depends on them
until a later boot reaches IOKit.

## Deferring T3, T4 and the full D1: risk for the first VM-stop diagnostic

Assumed: the run is attended, the package is `//boot:erista_first_xnu` with `switch_stop_after_vm`, the trace kernel and one core. T2 has run.

| Event | Without T3, T4, D1 | Residual risk |
| --- | --- | --- |
| The kernel reaches VM done and the timed reset works | resets after 15 s (or N), hekate returns | the call after XNU has not been tried; `wfe` under XNU's `cntkctl_el1` has not been shown to wake |
| It reaches VM done, the reset does not return | parked, no timer, no spin; hot only as far as the fan hekate left | the person holds POWER; the console stays warm until then |
| It panics before VM done | the screen stays (`reboot_s` 0); no reset on the panic path | the person; the console is not cooling harder |
| It hangs in `start.s` or early C | nothing acts | the person at 30 s; this is the case T4's watchdog would cover and it does not exist |
| Heat | no temperature is read by any layer; the fan is off at the loader (Nyx's launch ran `fan_set_duty(0)`) | the first window is loader seconds plus XNU's early init, then the reset. D1-min rose 4.1 C start to end across hekate, Nyx and a 15 s dwell. That is one observation, not a model of XNU's early init |
| SD card | not touched: the reset comes before IOKit and the SD driver | none from this run |
| Exporter | not used | none: D1's exporter is a later step |

Reading: deferring T3 and T4 means a hang or a failed reset has no autonomous stop and no fan, and the person is the only mechanism. That is the stated condition
of the run. T2 measured one temperature at the loader (remote 34.3125 C) and shows nothing about XNU's early heat. The full D1 adds nothing to this run's safety, so deferring it costs nothing here. What the owner decides, in writing, is whether a person's POWER
hold is accepted as the sole protection against a hang with the fan off, with start below the provisional 40 C and the 30 s and n + 10 s rules. Those figures are not
official limits. No part of this document lowers the HOLD.

A stronger statement is not available: nothing measured shows XNU's early heat or the reset after XNU.
