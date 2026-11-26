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
| 1 | Timed `SYSTEM_RESET` from `switch_diag_vm_done` | the VM-done stop, a stop in a stage after VM init, if the kernel still reaches the call | a fixed `smc`, no ADT change, the counter and `wfe` as the loader does, no polling | not built |
| 2 | Same call from the panic path of the debug kernel, after the record is saved | a kernel panic before VM init finishes | a call site in the panic code, behind `DEVELOPMENT \|\| DEBUG` | not designed further |
| 3 | WDT0 or WDT4 reset | a hang that never reaches layer 1 or 2 | the TRM's register sequence and its destination (hekate, or a loop) | blocked: no source for the A57's route |
| 4 | THERMTRIP | overheating | calibrated SOC_THERM, a PMC reset command the DTB lacks | out of reach |
| 5 | Fan (T3) | heat while parked | PWM clock on, rail GPIO and PMC writes, the tach | designed, unapproved |

Layer 1 is the only new one that rests on something measured. Layers 3 to 5 do not move.

## Layer 1 as it would be built

- In `SwitchVmDone.c`, under `DEVELOPMENT || DEBUG`, in place of `park()` when `switch_stop_after_vm=1`: mask interrupts, set `CNTKCTL_EL1`
  to the event stream value the loader uses (`0xf4`), `wfe` until the counter passes a deadline, then the same `smc #0` and a final park if it returns.
  `wfe` and the event stream are the loader's own no-poll wait, so the core is not spun.
- The deadline is a boot-arg (`switch_stop_after_vm=<seconds>`, replacing the flag), with no default above the loader's 15 s and a refusal
  of 0 or of more than 60. The screen says the number and that the console resets itself.
- Release kernels contain none of it (`first_xnu_check` and `vm_done_source_test` already pin the gating).
- The run sheet's recovery stays the held power button. The reset is an addition, not a replacement.

What it would prove, in one approved run: that the call lands in hekate from XNU's EL1. What it would not prove: anything about a
hang before VM init, or a panic loop.

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

## Decisions for the owner

1. Is layer 1 acceptable as the stop for the VM-done run, in place of "the power button in writing"?
2. May the deadline be a boot-arg, and is 60 s the right ceiling?
3. Should the panic path (layer 2) get the same call, or stay with the person?
4. Hold on layers 3 to 5 until a TRM or a measured dump exists. Nothing in this file asks to change that.
