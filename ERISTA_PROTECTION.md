# Autonomous protection for the first XNU run (T3/T4 design)

Design only. Nothing here is built or approved, `boot` stays 0 and every hold stays. It narrows `ERISTA_FIRST_XNU_BOOT.md` ("T3 and T4 as
safety mechanisms") and `ERISTA_STOP_PATHS.md` to one question: what can end the first run without a person.

## A finding that changes the stop-path table

`ERISTA_STOP_PATHS.md` says the kernel cannot reach PSCI because the ADT has no `/psci`. That is true of the kernel's own conduit, and
irrelevant to a diagnostic that issues `smc #0` itself. The D0 and D0-R loaders do exactly that at EL1
(`boot/diag/loader/exit.c`: `x0 = 0x84000009`, `smc #0`, after a counter wait) and the console returned to hekate both times
(`ERISTA_LOADER_DIAG.md`). XNU starts at that same EL1: the production loader includes the same `boot/handoff/entry.S`, which writes HCR_EL2 `0x80000000` (RW only, `TSC` clear, so `smc` goes to EL3, not to EL2).
So the one thing not shown is narrower than the table says: the call after XNU has run its own startup (MMU on, its own vectors,
caches in use). The firmware does not see any of that, but it has not been tried.

## Layers, weakest to strongest

| # | Layer | Ends | Needs | State |
| --- | --- | --- | --- | --- |
| 0 | The person holds POWER | anything | someone at the console | the only one that exists |
| 1 | Timed `SYSTEM_RESET` from `switch_diag_vm_done` | the VM-done ending | a fixed `smc`, no ADT change, the counter and `wfe` as the loader does, no polling | built offline, not run |
| 2 | Same call from the panic path | a kernel panic before VM init finishes | a call site in the panic code | not done: left with the person |
| 3 | WDT0 or WDT4 reset | a hang that never reaches layer 1 or 2 | the TRM's register sequence and its destination (hekate, or a loop) | blocked: no source for the A57's route |
| 4 | THERMTRIP | overheating | calibrated SOC_THERM, a PMC reset command the DTB lacks | out of reach |
| 5 | Fan (T3) | heat while parked | PWM clock on, rail GPIO and PMC writes, the tach | designed, unapproved |

Layer 1 is the only new one that rests on something measured. Layers 3 to 5 do not move.

## Layer 1 as built (offline)

Approved on 2026-11-26 for the first-XNU diagnostic only; the panic path (layer 2) was left out. The sequence, the boot-arg rules and the
failure screens are in `ERISTA_FIRST_XNU_BOOT.md` ("The timed reset"), the code in `SwitchVmDone.c` and `SwitchVmReset.h`.

Open items, none settled by a host test:

- The event stream under XNU. `cntkctl_el1` is written with the loader's event stream bits and `wfe` is expected to wake every 2^16 counts. If it does not,
  `wfe` never returns and the run ends only by the power button. The loader showed it working at EL1 before XNU's own setup, not after.
- The counter. Its frequency and advance are checked once before the wait, not during it. A counter that stops later is not detected.
- The call after XNU owns the core. Same firmware and same HCR_EL2, but not tried.
- What the reset does to RAM and to the screen, and whether hekate comes back, as in the loader runs.

The reset ends one observation. It is not a hang or temperature protection and does not replace either.

## What each layer leaves unprotected

| Event | 0 | 1 | 2 | 3 to 5 |
| --- | --- | --- | --- | --- |
| Reaches VM done, then idles | person | resets | | |
| Panics before VM done | person | | resets | |
| Hangs in start.s or early C | person | | | none |
| Runs hot while parked | person | bounds the time | | fan (T3) only |

The second and third rows are why the 30 s no-progress limit is still the person's rule.

## Temperature, honestly

The fan is off at the loader (Nyx's launch ran `fan_set_duty(0)`), so heat while parked is bounded only by time. Layer 1 bounds the
parked time. The loader's phase before XNU is a few seconds. No temperature is read by any layer, and the 40, 50 and 55 C figures
remain provisional criteria for the person, not triggers.

## Decisions

1. Layer 1 is approved as the VM-done ending. The 15 s default and the 60 s ceiling are settings, not safe durations.
2. The wait is a boot-arg, 1 to 60 s.
3. The panic path stays with the person.
4. Hold on layers 3 to 5 until a TRM or a measured dump exists.
