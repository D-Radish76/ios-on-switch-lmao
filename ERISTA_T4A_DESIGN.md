# T4-A: detailed design on paper

Written 2026-11-27 after the first T4-R run (`ERISTA_T4R_RESULT.md`). Nothing here is built, implemented or approved. No code, no allowlist, no image, no new MMIO write, no watchdog armed. `ERISTA_T4_DIAG_PLAN.md` has the three stage outline; this file works T4-A through and lists what is still unknown. The register facts are those of `ERISTA_T4_WATCHDOG_DESIGN.md`.

## What the first T4-R run changed

| Fact | Effect on the plan |
| --- | --- |
| The four words read without a hang at EL1 | the read side of T4-A's precondition is met. Writes are a different question and are still untested |
| `TMR5_PTV`, WDT0 config and status read zero (IDLE) | the precondition "idle at T4-R" holds for WDT0 on timer 5 after hekate's init |
| `TMRUS` is a microsecond counter | `TMR5_PTV` periods can be written in microseconds and a loader wait can be checked against `TMRUS` |
| the return to hekate took the PSCI path again | the loader's exit is still timed PSCI, so a T4-A that parks and waits for a reset does not use it |

Bits 0 and 1 of the status stay unnamed: a zero word says nothing about them.

## Image

One image, `diag_t4a`, a new target like `diag_t4r`, own sources, own allowlist, own audit file, entry `ios-diag-t4a`. The ten diagnostic hashes stay as they are. It arms WDT0 on timer 5 only. WDT4 is not touched (hekate arms WDT4 itself and ends it before the launch).

## Sequence

1. Draw the title, check EL1 and the board as every stage does. A board that is not Erista ends the stage with no register touched.
2. Read the T4-R set, each read drawn and settled first. Continue only if the state is IDLE with `TMR5_PTV`, config and status all zero. Anything else (BUSY, DORMANT, CONFIGURED_STOPPED, UNKNOWN) ends the stage with the state on screen. Nothing is written.
3. Draw `ABOUT TO ARM WDT0 PERIOD <T_A> S`, settle it, then program, one gate call per write, each drawn first: unlock `0x6000510c` with `0xc45a`; command `0x60005108` with `2` (disable) then `1` (start); `TMR5_PTV` `0x60005060` with the one periodic word; config `0x60005100` with the one word `5 | (P << 4) | (1 << 15)`. The exact order and values come from the register table of `ERISTA_T4_WATCHDOG_DESIGN.md` and are checked against the panic path (`diagnostics/switch/SwitchPanicSave.c`) before any code.
4. Read back `TMR5_PTV`, config and status and draw them. If config or `TMR5_PTV` differ from what was written, run the stop sequence (below) and end with the reason.
5. If they match, draw `ARMED, RESET EXPECTED IN ABOUT <T_A> S` once and park in a `wfe` wait with no further register access and no PSCI call. The person times the video.

Stop sequence, one pass, never repeated: unlock, command `2` (disable), `TMR5_PTV` 0, read back, draw. It runs only when the read back failed, and it does not run after step 5.

## Gate

A write gate beside the read gate. It accepts exactly the four write targets above with exactly the words above (`0xc45a`; `1` and `2`; the one `TMR5_PTV` word and 0; the one config word and 0), and refuses everything else, including every WDT4 address and every PMC or CAR address. Its model test runs every 32-bit value for each target and passes only the listed words, and counts zero writes after a stop. The audit lists each write once, with the function that makes it, and fails on a store anywhere else. All of this is design; no test exists.

## Period

`T_A` is the owner's choice and is not a safety limit. The values the repository already uses are 45 s (shutdown watch), 48 s (heartbeat) and 60 s (panic path). It must be long enough to read the screen and reach POWER and short enough that the stop time is tolerable. The run sheet would give the stop figure as `T_A` plus 15 s, provisional, and the console is parked and idle in that wait.

## Unresolved, each needs an answer before T4-A is asked for

| # | Open item | What would answer it |
| --- | --- | --- |
| 1 | Where the console lands after a watchdog reset. The PSCI path returns to hekate by a mechanism that is not verified to apply to a watchdog reset; the console may boot something else or stop at RCM | the owner's own knowledge of the console, or a cautious observation; nothing in this tree answers it. This is the first item because the recovery plan depends on it |
| 2 | Writes at EL1: reads answered, writes may fault or be ignored | only T4-A itself shows it; the stop path for a fault must be defined |
| 3 | Which timer and registers the hardware watchdog uses: mainline Linux and the panic path say timer 5 at `0x60005100`; L4T's DTB says index 7 at `0x60005088` | the TRM (not in this repository) or a mainline check; T4-R read the former and found them idle, which names no wiring |
| 4 | Status bits 0 and 1 for WDT0, and the expiry count | the read back after arming would show them; no source here |
| 5 | The PMC and BPMP part of the reset (`1 << 15`, PMC2CAR) | the TRM; hekate zeroes the reset status, so the cause cannot be read afterwards |
| 6 | Whether the TKE state survives the reset (design outcome 3) and so a loop | the T4-R that follows T4-A; a loop is stopped by POWER |
| 7 | The time from reset to hekate on the PSCI path (an impression of 15 s from COMPLETE, which does not fit the exit path) | the video of a later run, read with the four clocks named in `ERISTA_T4R_RUN.md` |
| 8 | The temperature during a run | a reading taken during the run, not only before and after |

## Where T4-A stands against the transition conditions

From `ERISTA_T4R_RUN.md`: a run that passed, with the raw words and the state recorded (the words and state are recorded; the formal pass is not claimed, `ERISTA_T4R_RESULT.md`); WDT0 answering at EL1 without a hang (met for reads); status bits 0 and 1 meaning, the timer wiring (items 3 and 4, open); the measured time from COMPLETE to hekate (item 7, open); the owner's separate written approval of its writes, allowlist, recovery and loop risk (not given). T4-A is not ready, and this file does not ask for it.
