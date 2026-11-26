# T4 diagnostics: three stages for a SoC watchdog

Plan of 2026-11-27, written before any loader code. Nothing here is implemented, built or approved: no new MMIO, no watchdog operation, no change to an allowlist, `boot` stays 0 and the feature mask 0, T3 and T4 are not waived, and the first XNU run is on HOLD. The design is in `ERISTA_T4_WATCHDOG_DESIGN.md`. `ERISTA_T4R_DESIGN.md` (2026-11-27) overrides this plan where they differ: a smaller read set, and no use of `PMC_RST_STATUS`, which hekate clears before any loader runs. Where a register's meaning is not in a source named there, the stage does not operate on it.

Each stage is its own diagnostic image, built like T2 (`boot/diag/loader`, an allowlist gate, model tests, a Mac build with its hashes recorded before the run). They are new targets, so the nine diagnostic hashes of `diag_images.py` do not change. A stage is run only after the one before it passed and the owner approved it in writing. The common conditions are those of T2 (`ERISTA_T2_RUN.md`): hekate 6.5.5, cold start, Nyx temperature written down and below the provisional 40 C, video on, a person at the console with a hand on POWER, the card holding only that entry.

`PMC_RST_STATUS` cannot serve as evidence in any stage, because hekate clears it in `hw_init`; the stages pass on timing, hekate's menu and the next T4-R's `TMRUS`.

Order: T4-R, then T4-A, then T4-R again to read what T4-A left, then T4-P, then T4-R again. A stage that shows the loop outcome (below) ends the series.

## T4-R: read the state, write nothing

| | |
| --- | --- |
| Purpose | the state of both TKE watchdogs and their timers before anything arms them, and, through `TMRUS`, whether the SoC reset since hekate started. It also shows whether the WDT registers answer at EL1 here, which nothing in the repository shows |
| Reads (32-bit) | `TMRUS` `0x60005010` (twice, to see it advance); tier 1: `TMR5_PTV` `0x60005060`, WDT0 config `0x60005100` and status `0x60005104`; tier 2: WDT4 config `0x60005180` and `TMR9_PTV` `0x60005080`. The CAR words are not read (measured already: TKE clocked and out of reset); `PMC_RST_STATUS` is not read (hekate zeroes it). Reasons in `ERISTA_T4R_DESIGN.md` |
| Never read | the command and unlock registers (`0x108`, `0x10c`, `0x188`, `0x18c`): not described in any source here, and a read could act |
| Writes | none. The model test counts zero |
| Allowlist proposal | the registers above, read only, 32 bits only; any other address or width is refused and reported as a loader bug, as T1 and T2 do |
| Order | `TMRUS`, then tier 1, then tier 2, one gate call each. The first read that does not return ends the stage: nothing is read after it |
| Stop | no screen change for 30 s (provisional): hold POWER. A refused read, a bad width or a counter that does not advance ends the run with the cause on screen |
| Pass | every read returns; `TMRUS` advances; each WDT is classified by the heartbeat's rule (idle, dormant on a stopped timer, busy); the values are on the screen and photographed; the stage ends with the same timed `SYSTEM_RESET` as D0-R and returns to hekate |
| Recovery | hekate after the reset; otherwise hold POWER, cold start with the console off for 10 s |
| Loop risk | none from this stage: nothing is armed. A watchdog found already armed (busy) is reported and left alone; if hekate then resets by itself, power-cycle and stop the series |
| What it does not show | that a write to these registers works, that a reset follows one, or anything about the BPMP |

## T4-A: arm, expire, return to hekate

Precondition: the T4-R before it found the chosen watchdog idle, or dormant on a stopped timer in a way the owner accepts, and its registers answered.

| | |
| --- | --- |
| Purpose | whether arming the chosen watchdog from the loader resets the SoC after the period and lands in hekate, and what the TKE block looks like afterwards (read by the next T4-R) |
| Choice of watchdog | the design's provisional preference is WDT0 on TMR5; WDT4 on TMR6 is the alternative. The owner chooses after T4-R. One stage image arms one |
| Writes (WDT0 shown; WDT4 is the same at `+0x80`) | unlock `0x6000510c` with `0xc45a` only; command `0x60005108` with `2` then `1` only; `TMR5_PTV` `0x60005060` with `0` or the one periodic word (`(1<<31) \| (1<<30) \| 249999`) only; config `0x60005100` with the one word `5 \| (P << 4) \| (1 << 15)` where `P` is fixed at build time, nothing else |
| Reads | the T4-R set, and the config, status and `PTV` read back after arming |
| Period | `P` and the timer tick fix the total (about `P` seconds for the panic path's word). The owner picks `T_A`. The values the code already uses are 45 s (the shutdown watch), 48 s (the heartbeat) and 60 s (the panic path). It must be long enough to read the screen and reach POWER. It is not a safety limit |
| Order | the T4-R reads, classify, program (unlock, disable, `PTV`, config, start), read back; if the read back does not match, run the stop sequence (start to clear, unlock, disable, `PTV` 0) and end with the reason; if it matches, print `ARMED, RESET EXPECTED IN ABOUT <T_A> S` once and park in a `wfe` wait with no further register access and no PSCI fallback |
| Stop | no reset by `T_A` plus 15 s (provisional): hold POWER. The loader is parked and idle, so this is the failure case with the least heat |
| Pass | the console resets near `T_A` (timed on video), returns to hekate, and the next T4-R shows the TKE state and a `TMRUS` that restarted with the reset. No second reset in the 3 × `T_A` after hekate is up, with the person doing nothing in its menu |
| Recovery | hold POWER (a hard off), wait 10 s, cold start. If the console loops, hold POWER at the first second reset |
| Loop risk | the first place a loop can appear. If the TKE block keeps its configuration and its count across the reset (outcome 3 in the design), hekate is reset every `T_A` and the person must power it off. Hekate's `watchdog_fired()` reads an IRAM word set by its own FIQ handler, so a bit-15 reset does not trigger its skip path. Nothing but the loader and hekate runs, so no XNU and no SD write are involved |
| What it does not show | the reset after a PSCI `SYSTEM_RESET`, anything about XNU, the BPMP being dead or hung |

## T4-P: what a PSCI reset leaves behind

Precondition: T4-A passed, and its follow-up T4-R showed the block idle or dormant (outcome 1 or 2). If T4-A showed outcome 3, the series stops here and the design is reworked.

| | |
| --- | --- |
| Purpose | the stop the first run really uses is the timed PSCI `SYSTEM_RESET` at VM done. If a watchdog is armed before the jump and the run ends that way, does the armed state survive and reset hekate later? This stage arms, then resets by PSCI, and reads what is left |
| Writes | the T4-A writes, same values, same gate. No new register |
| PSCI | the loader's own `smc` with `SWITCH_PSCI_SYSTEM_RESET`, as D0-R and T2 end. The call is made once and is not retried |
| Period | `T_P`, at least `T_A`, chosen so that the person can watch hekate for `T_P` plus 15 s with a hand on POWER. Not a safety limit |
| Order | T4-R reads, classify, arm, read back, `SYSTEM_RESET`. After hekate appears the person does nothing for `T_P` plus 15 s, then launches the T4-R image |
| Stop | a second reset inside that window means the armed state survived the PSCI reset: record the time since hekate came up, hold POWER, end the series. No return to hekate after the `smc`: hold POWER |
| Pass | no reset in the window, and the T4-R that follows shows the block idle (outcome 1). Outcome 2 (configuration kept, count stopped) passes only if the XNU run's design disarms before the reset or states why it need not; the owner decides |
| Recovery | hold POWER, cold start. A power-cycle clears the block only if power is lost, which is not shown here; if hekate keeps resetting after a cold start, stop and report |
| Loop risk | the highest of the three: the arm is real and the reset is the ordinary stop. It is the stage that decides whether the XNU run's normal ending can leave a loop |
| What it does not show | that XNU's early code leaves the block alone, or that the reset after XNU owns the core works (row 5 of `ERISTA_FIRST_XNU_BLOCKERS.md`) |

## Allowlist proposal, collected

For the owner's review, not for a build. All accesses are 32-bit.

| Register | Address | T4-R | T4-A | T4-P |
| --- | --- | --- | --- | --- |
| `TMRUS` | `0x60005010` | R | R | R |
| `TMR5_PTV` (WDT0) or `TMR6_PTV` (WDT4); `TMR9_PTV` | `0x60005060`, `0x60005068`; `0x60005080` | R | R, and W of the chosen timer's PTV with `0` or the one word | same as T4-A |
| WDT0 config, status | `0x60005100`, `0x60005104` | R | R, config W with the one word | same |
| WDT0 command, unlock | `0x60005108`, `0x6000510c` | none | W: command `{2, 1}`, unlock `0xc45a` | same |
| WDT4 config, status | `0x60005180`, `0x60005184` | R | as WDT0 if WDT4 is chosen, else R | same |
| WDT4 command, unlock | `0x60005188`, `0x6000518c` | none | W if WDT4 is chosen | same |

No PMC access at all, no CAR access, no I2C, no pinmux, no GPIO. The write gate's model test is exhaustive over the 32-bit values of each writable register and passes only the listed words, and nothing is written after a stop, as T2's was.

## Evidence this plan lacks

The TRM is not in this repository. Not known: whether the WDT registers are accessible at non-secure EL1 here, which source index each watchdog takes (the DTB and the panic path disagree), what the expiry count and a period of zero do, what the PMC needs for the reset to act, and the BPMP's part. T4-R is the only stage that does not depend on any of these, and it is the first. If it shows a register that does not answer or a state that does not fit the heartbeat's classification, the later stages are redesigned, not run.
