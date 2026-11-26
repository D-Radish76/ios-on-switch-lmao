# First XNU diagnostic: run sheet

For the run once the owner approves it in writing. Nothing here approves it. `boot` stays 0 and the feature mask stays 0 until the owner sets the row for this build
(`ERISTA_FIRST_XNU_BOOT_GATE.md`). T3, T4, the full D1 and every other XNU run stay on HOLD. The T3/T4 waiver is undecided. D1 is not a condition (owner's decision).

The run ends at `arm_vm_init` returning, then a timed PSCI `SYSTEM_RESET` (15 s by default). One run, no retry, a person at the console with POWER the whole time. No autonomous hang
protection exists; no temperature is read by any layer during the run.

## Before the run

| # | Check | Where |
| --- | --- | --- |
| 1 | The Mac passed every command of `ERISTA_FIRST_XNU_MAC.md` section 7 on one commit and reported the figures | `ERISTA_FIRST_XNU_MAC.md` |
| 2 | Owner's written decisions: the run itself; the stop path (the person, plus the timed reset, which is untried after XNU owns the core); the seed; `boot` 1 for this build only; T3/T4 deferred or waived | `ERISTA_FIRST_XNU_BLOCKERS.md` |
| 3 | The package is `//boot:erista_first_xnu_seeded`, built with the trace kernel through the seed wrapper; the two must-fail builds failed | `ERISTA_FIRST_XNU_MAC.md` sections 2, 7 |
| 4 | Loader margin reported: flat 122880 or less, `to_stamps_slot` `0xf00` or more, `to_sd_reference` `0x1000` or more | `first_xnu_check.py margin` |
| 5 | The card holds only this entry's files, hashed on the Mac, copied, hashed again after a re-insert; no `._` files; `iOS` is not in hekate's list | `ERISTA_FIRST_XNU_BOOT.md` "Real-hardware preparation" |
| 6 | The normal Erista package is not on the card | `ERISTA_FIRST_XNU_MAC.md` section 1 |
| 7 | hekate 6.5.5; cold start (off 10 s); Nyx temperature written down before selecting the entry, below the provisional 40 C; battery charged, console on a table, Joy-Con off | as D1-min and T2 |
| 8 | Video on from the first U-Boot line; someone has read the stops below | |

## What the screen shows

U-Boot's D0 lines, the loader's report, green marks 1 to 21 at y 280 (marks 1 to 6 from `start.s`, 7 to 21 from the C stages), then `XNU VM INITIALIZED` and
`ARM_VM_INIT RETURNED AFTER STAGE 21`, `RESETTING IN <n> S. HOLD POWER TO STOP`, then the reset. Mark 21 without the text means the kernel died between stage 21 and the return. A fault replaces the marks with the early exception
screen, whose LAST STAGE line names the stage. If no mark appears the screen cannot tell a loader hang from a boot-args mismatch. The loader's stamps at `0xa001ef00` are the second source.

## Stops

| Condition | Action |
| --- | --- |
| A panic, an exception screen, garbage or black | hold POWER; photograph first if the screen is readable |
| No new mark for 30 s | hold POWER (provisional) |
| `RESETTING IN <n> S` shown and the console has not reset n + 10 s later | hold POWER (provisional). The line is drawn once and does not count down; time it from when it appears. `wfe` waking under XNU's `cntkctl_el1` and the reset call after XNU are what this run tests; if they fail only POWER ends it |
| `SMC RETURNED <x0>. NO RETRY` or `NO RESET` on screen | the core parks; hold POWER |
| Nyx showed 50 C or more at the start of any attempt, or the console is hot | do not start; hold POWER if running (provisional, not official limits) |

## Why there is no total time limit

The earlier 120 s limit from the loader's start is withdrawn. Nothing measured says that 120 s with the fan stopped is safe, and a limit that long was
never a protection. The two rules above are tied to events instead. A hang before the VM text is cut 30 s after the last mark. After the text the reset is due `n`
seconds later (`n` is `vm_reset_seconds`, 15 unless the owner sets it, 1 to 60), so the longest the console sits parked is `n + 10` s. The 10 s covers the call and hekate's return and is a guess. Marks that
arrive slower than every 30 s are not bounded by these rules; the person may stop earlier at any time. The record gets the selection-to-hekate time, which no run has measured, so the next sheet can use a measured figure.
A shorter `n` shortens the parked time and costs the time to photograph the screen; that setting is the owner's.

### Marks that keep trickling in

The 30 s rule restarts at every mark, so a kernel that advances slowly is never stopped by it. The only bound is arithmetic: there are 21 marks, so the longest the rules allow
before the VM text is 21 x 30 s, 10.5 minutes, plus the 30 s after mark 21, then `n + 10` s. That is a figure about the rules, not about the console: no run has measured the time between marks, whether marks can repeat or come back,
or how hot the console gets in that time, and the fan is not running. It is not a safety limit and must not be used as one.

What protects the console is the person at it, and that is not shown to be enough either. Nothing in the package reads a temperature, watches the fan or detects a hang inside the VM setup, and
the PSCI reset after XNU owns the core is untried. So the operator rule is to hold POWER at the first reading that is not on this sheet: a pause that looks long, a mark out of order or repeated, the screen redrawing,
a mark that arrives much later than the ones before it. The marks' timing is for the report: write down when each mark appears (a phone video with the clock does it), so the next sheet can use measured gaps.

## Held: the run condition for trickling marks

The owner holds back using "ten minutes or more with the fan stopped" as a condition of the first run. Three ways to change that, none adopted, none a safety guarantee:

| Option | Risk | What it takes |
| --- | --- | --- |
| A total time limit (for example from the first mark) | Any number is a guess: no run has measured the marks' timing, XNU's early heat or the fan-off time the console tolerates. It would be a rule for a person to follow, not a protection: a hang does not look at a clock, and a person who misses it misses the limit. A limit also cuts a diagnosis short that was only slow | Sheet text only. Making the package enforce it needs a timer that works while the kernel is hung: the watchdog (T4) or a reset source, both unbuilt, unproved and not approved |
| A shorter wait after VM done (`vm_reset_seconds` below 15) | Shortens only the time between the VM text and the reset. A hang before the text and trickling marks are untouched, and the reset after XNU owns the core is untried, so a short `n` fails the same way a long one does. A very short `n` leaves no time to read or photograph the screen | A boot-args value (`switch_stop_after_vm`, 1 to 60); no code. The setting is the owner's; filming the screen replaces reading it |
| Fan control, or a hang protection independent of the kernel | The only one of the three that acts when nobody does. Also the one with hardware risk: a fan or watchdog write that is wrong on an Erista is itself a failure. Fan state at hekate's handoff is not recorded; Nyx's launch ran `fan_set_duty(0)` | T3 (fan) and T4 (watchdog) as designed in `ERISTA_D1_DESIGN.md`: new register code in the loader, a new MMIO allowlist entry, a design review, a run on the console for each. All on HOLD, unbuilt and unapproved. THERMTRIP, WDT, fan and voltage operations stay forbidden until then |

None of these is chosen here. Until the owner chooses, the sheet's stop rules stand as written and carry the limit stated above: the person is the only protection, and the run is not approved.

## Seed

The run uses the per-install seed (owner's stated priority): `//boot:erista_first_xnu_seeded`, built only through `erista_seeded_build.py`
(`ERISTA_FIRST_XNU_MAC.md` section 6). The card's copy holds the seed, so the card is not shared and the entry's files are removed afterwards. Reports carry the 8-digit fingerprint and file hashes, never the seed.

## Afterwards

Photograph first, then remove the entry's files from the card, then record Nyx's temperature. Report: the video, the last mark, whether the text and the reset came, the Nyx start and end
figures, the selection-to-hekate time. A screen that differs from this sheet is a finding to send, not to explain. A second run needs a new written approval, whatever the result.
No result of this run shows XNU's SD driver, IOKit, the GIC, userspace or the heat of a longer run.
