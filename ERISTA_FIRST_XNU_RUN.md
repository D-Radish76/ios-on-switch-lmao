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

## Seed

The run uses the per-install seed (owner's stated priority): `//boot:erista_first_xnu_seeded`, built only through `erista_seeded_build.py`
(`ERISTA_FIRST_XNU_MAC.md` section 6). The card's copy holds the seed, so the card is not shared and the entry's files are removed afterwards. Reports carry the 8-digit fingerprint and file hashes, never the seed.

## Afterwards

Photograph first, then remove the entry's files from the card, then record Nyx's temperature. Report: the video, the last mark, whether the text and the reset came, the Nyx start and end
figures, the selection-to-hekate time. A screen that differs from this sheet is a finding to send, not to explain. A second run needs a new written approval, whatever the result.
No result of this run shows XNU's SD driver, IOKit, the GIC, userspace or the heat of a longer run.
