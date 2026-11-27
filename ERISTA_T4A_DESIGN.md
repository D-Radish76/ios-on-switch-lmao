# T4-A: detailed design on paper (revised 2026-11-27 after the reset-path study)

Nothing here is built, implemented or approved. No code, allowlist or image changes; no watchdog is armed; no new MMIO write exists; T4-R is not run again. The first T4-R run is in `ERISTA_T4R_RESULT.md`; the reset paths are in `ERISTA_T4A_RESET_PATH.md`. `ERISTA_T4_DIAG_PLAN.md` has the three stage outline, which this file supersedes for T4-A. Marks: **confirmed** (read in a named source), **inferred**, **unknown**.

## What changed since the first version

| Fact | Effect |
| --- | --- |
| The four words read at EL1 without a hang (T4-R run 1) | reads are accepted for this block; writes remain untested |
| All of `TMR5_PTV`, WDT0 config and status were zero after hekate's init | the idle precondition holds for WDT0 on timer 5 |
| `TMRUS` runs at 1 MHz: hekate writes `TIMERUS_USEC_CFG` `0x45F` ("for 19.2MHz clk_m") in `hw_init`, and the run measured 1010578 us across about ten 100 ms steps | `TMR5_PTV` periods are in microseconds |
| BL31's PSCI reset prepares the return to hekate; a watchdog reset does not (`ERISTA_T4A_RESET_PATH.md`) | **the first design's pass condition "returns to hekate" is withdrawn.** After a WDT0 expiry the console is expected to go where a cold start goes, which only the owner knows |
| Mainline `tegra_wdt.c` matches only `nvidia,tegra30-timer`, and the fetched mainline `tegra210.dtsi` lists `nvidia,tegra210-timer` alone for `timer@60005000` | earlier documents say mainline Linux uses WDT0 with timer 5. What is confirmed is that the driver's constants are those; that it binds on T210 is not shown by these two files |

## Watchdog implementations compared

| | Mainline `tegra_wdt.c` | L4T 4.4 `tegra_wdt.c` and the pinned DTB | hekate v6.5.5 `timer.c` | This tree (panic path; heartbeat) |
| --- | --- | --- | --- | --- |
| Watchdog | WDT0 at `+0x100` (config 0, status 4, command 8, unlock `0xc`) | the same layout; DTB `watchdog@60005100`, compatible `nvidia,tegra-wdt-t21x`, `status` disabled | WDT4 (`TIMER_WDT4_*`), source TMR9 | WDT0 (panic path), WDT4 on TMR6 (heartbeat) |
| Timer source and PTV | source 5, PTV `+0x60` | 4.4 driver: source constant 5, the timer block from the second memory resource. DTB: `nvidia,timer-index` 7, second `reg` `0x60005088` (size 8). The L4T 28.2 page: "WDT0 is configured to use TIMER7" | TMR9 | source 5, PTV `0x60` |
| Arm order | PTV `(1000000/4) \| EN \| PERIODIC`, config `5 \| (timeout << 4) \| (1 << 15)`, command `1` (START) | the same | unlock `0xc45a`, PTV, config, command START | PTV, config, command `1` (`SwitchPanicSave.c`); the heartbeat first adds unlock and DISABLE |
| Stop order | unlock `0xc45a`, command `2` (DISABLE), PTV 0 | the same | PTV 0, unlock, command START (clears interrupts), command DISABLE, `TMR9_PCR` interrupt clear | unlock, DISABLE, PTV 0 |
| Expiry | "the 4th expiration" (L4T 4.4 comment); the DTB has `nvidia,expiry-count` 4 | the same | FIQ only, ended before the launch | the same |
| Who runs it | in mainline for Tegra30-era SoCs by the match above | the L4T driver; the t21x one was not read | before the launch | a kernel that is not booted in a stage |

Nothing in BL31 touches the watchdog (one TKE constant, `TMRUS`).

## The timer wiring disagreement

Three pairings of source index and PTV address exist:

- index 5 with PTV `0x60005060`: mainline, L4T 4.4, this tree's panic path, and the table in `heartbeat.h` (its `offsets[]` maps index 5 to `0x60`);
- index 7 with PTV `0x60005088`: the pinned DTB, and the L4T 28.2 page for the index;
- the `heartbeat.h` table (TRM table 20, as cited there) maps index 7 to `0x70` and puts `0x88` at index 0.

So the DTB's own pair disagrees with that table, and the table agrees with mainline. The vendor documents point at timer 7 and the upstream and this-tree code at timer 5. T4-R read `0x60005060` and found zero, which does not tell which timer the hardware watchdog counts. Only the TRM, or a measurement on a running timer, settles it. A read of the other candidate PTV words would be new MMIO and is not approved. Until it is settled, T4-A's chosen source is a provisional choice, and a wrong source could mean the watchdog counts a timer that is not running (no reset) or one that runs at another rate (a reset at another time). The stop figure has to cover both.

## Write order

Three orders exist (table above). The design proposes the shortest one, the mainline and panic path order, because T4-A starts only from a state read as idle and so needs no preliminary DISABLE:

1. `TMR5_PTV` `0x60005060` = one periodic word;
2. config `0x60005100` = `5 | (P << 4) | (1 << 15)`;
3. command `0x60005108` = 1 (START).

Stop, only on a failed read back, in this order: unlock `0x6000510c` = `0xc45a`, command `0x60005108` = 2 (DISABLE), `TMR5_PTV` = 0. The heartbeat's longer order (unlock and DISABLE first) and hekate's order (unlock first, its stop with a START to clear interrupts) are noted and not used: each adds writes, and a write is a place to hang. If the owner prefers the longer order, the gate changes; nothing else does.

Unknown: whether config is ignored while the counter runs (the heartbeat's comment, from Linux), so an interrupted arm leaves a state only a read back shows.

## PMC2CAR scope and the BPMP

Unknown. The only source for bit 15 is its name and the three code comments in this tree and Linux; the TRM is not here. BL31's own reset does not use it (`MAIN_RST`). The L4T 28.2 page says that expiry resets the CPU, and does not say the SoC. In a loader stage the BPMP firmware is not running (`NOTES.md`: BL31 powers it off on the Erista when hekate hands it an SC7 entry firmware), so a PMC2CAR reset that needed live BPMP software would not happen; that dependence is not known either way.

## Does the setting survive a reset

Unknown. Inferred: the PMC scratch registers survive a reset, because BL31 writes words there for the boot after it. hekate's `watchdog_end` leaves WDT4 "enabled on a stopped timer" (the heartbeat comment), so a disabled counter keeps its configuration across hekate's use. Nothing says what a SoC reset does to the TKE block. The next read after a reset is the only evidence, and it needs the console to reach something that can read (see unresolved item 1).

## A hang at each step

A read or a write that never completes cannot be left by software. Each step is drawn and settled before it is made, so the screen names the last one begun.

| Step that hangs | State left | Recovery |
| --- | --- | --- |
| a read of the idle check | nothing written, nothing armed | hold POWER |
| write 1 (`TMR5_PTV`) | timer 5 may run, no watchdog counts it | hold POWER; the next T4-R shows what stayed |
| write 2 (config) | PTV set, config set or not, command not issued: not counting | hold POWER |
| write 3 (START) | the counter may be running | the expiry may reset the console by itself and end the hang; otherwise hold POWER. This is the case where the watchdog helps |
| the read back after START | armed | as for write 3 |
| a stop-sequence write | possibly armed, possibly unlocked | hold POWER; the stop sequence is also what would have ended a running count |
| after the park, no reset | armed, loader parked | hold POWER at `T_A` plus the margin |

Power removal is the only certain way out. Whether it clears a half-armed TKE block is unverified.

## The gate: order and state, not only values

A value allowlist would accept the right words in the wrong place. The gate is a small state machine over the write targets, in `t4a_gate.c`, with its own model test over every 32-bit value:

| State | Allowed next | Moves to |
| --- | --- | --- |
| `CHECKED` (the T4-R reads done, idle, line settled) | write 1 with the one periodic word | `PTV_SET` |
| `PTV_SET` | write 2 with the one config word | `CONFIG_SET` |
| `CONFIG_SET` (config read back equal) | write 3 with 1 | `ARMED` (once, ever) |
| `ARMED` | reads; the park | none; no write is accepted |
| any state after write 1 | the stop sequence, only after a failed read back, in its order | `STOPPED` |
| `STOPPED`, `REFUSED` | reads | none; no write is accepted |

Rules: a write is accepted only if the previous step's line is drawn and settled; the START write is refused unless the read back of config and `TMR5_PTV` equals what was written; the arm happens once per run (a `.data` flag); any other address, value or width, any write from a wrong state, and every WDT4, PMC and CAR address is refused, ends the run with the refusal on screen and runs no stop sequence unless the state is after write 1. The audit lists each write once with its function and fails on any other store. All design: no gate, test or audit exists.

## Image and sequence

One image, `diag_t4a`, new target, own sources, allowlist and audit, entry `ios-diag-t4a`; the ten diagnostic hashes stay as they are. WDT0 only; WDT4 is not touched. Sequence: title, EL1 and board checks (not an Erista: no register touched); the T4-R read set, each read drawn first; continue only on IDLE with all three words zero; draw `ABOUT TO ARM`; writes 1 to 3 each drawn first with the gate; read back and draw; `ARMED, RESET EXPECTED IN ABOUT <T_A> S` once; park in `wfe` with no register access and no PSCI call.

## The period is not taken from existing values

The 45 s, 48 s and 60 s of the shutdown watch, heartbeat and panic path are timeouts chosen for a running kernel. They are not safety limits, none was ever run on an Erista here, and none is the basis of `T_A`. `T_A` is the owner's choice from three things: the time the person needs to read the lines and reach POWER (to be taken from a video of the first run), the longest wait the owner accepts with the console parked, and the unit's meaning, which is not measured: the period field's unit follows Linux's comment (a 250 ms tick, four expiries), and a mistake of that kind scales `T_A`. A time limit is not a safety guarantee.

## Unresolved, with the source behind each

| # | Item | Status | Grounds | What would settle it |
| --- | --- | --- | --- | --- |
| 1 | Where the console lands after a WDT0 expiry | unknown | BL31 and hekate do the return only on the PSCI path (`ERISTA_T4A_RESET_PATH.md`) | the owner's description of a cold start, then an observation |
| 2 | How the owner's console starts hekate from cold (modchip, payload on the boot path, jig and injector) | unknown to this tree | none | the owner |
| 3 | Where `R2P_PAYLOAD_BASE` is and whether any stage can overwrite it | unknown | `l4t_defs.h` not read | hekate source; matters to every PSCI stop |
| 4 | Timer wiring: source 5 at `0x60`, or 7 at `0x88` | unknown | mainline and this tree against the DTB and L4T 28.2; the `heartbeat.h` table agrees with neither DTB pair | the TRM; or a read-only extension of T4-R (needs approval) |
| 5 | Whether mainline's driver binds on T210 | unknown | `tegra_wdt_of_match` against `tegra210.dtsi` | a mainline check |
| 6 | The `nvidia,tegra-wdt-t21x` driver's write order | unread | the DTB names it; the L4T 4.9 source was not fetched | the Switchroot kernel source |
| 7 | Writes at EL1 | unknown | reads answered; BL31 never touches the block | T4-A itself |
| 8 | Status bits 0 and 1 for WDT0; the expiry count | unknown | none; the zero word names nothing | the read back after arming |
| 9 | The scope of the PMC2CAR reset (CCPLEX or SoC) and the BPMP's part | unknown | the L4T 28.2 page speaks of resetting the CPU; BL31's reset is `MAIN_RST` | the TRM |
| 10 | Survival of the TKE setting across a reset | unknown | see above | the read after a reset |
| 11 | A boot ROM watchdog (`WDT_DURING_BR`) | unknown | hekate's `hw_init` comment | the TRM |
| 12 | The unit of the period | inferred | Linux comment: 250 ms tick, four expiries; not measured | the video of T4-A |
| 13 | The temperature during a run | unmeasured | only before and after were taken | a reading during the run |
| 14 | PSCI after an armed watchdog (T4-P) | out of scope here | `ERISTA_T4_DIAG_PLAN.md` | not designed here |

## Conditions before T4-A could be approved

All must hold and be recorded; the owner approves in writing.

1. Items 1 and 2 answered: the owner says how the console starts and where it lands after a cold reset, and agrees to the recovery for a landing outside hekate.
2. The timer wiring (item 4) settled from the TRM or by an approved read-only step, or the owner accepts a provisional source in writing.
3. The write order chosen from the table above, and the gate, its model test and its audit written and passing, with no store outside the gate.
4. `T_A` chosen by the owner by the rule above, with the video timings of the first run in hand (a formal T4-R pass needs them too).
5. The loader image's ten earlier hashes unchanged and `diag_t4a` built on the Mac with its hashes recorded.
6. A run sheet (stop conditions, the temperature during the run, the card hash before and after) like `ERISTA_T4R_RUN.md`.
7. The owner's separate written approval of: the four writes, the stop sequence, the recovery by POWER hold, and the loop risk.

Standing limits: T4-R not run again, T4-A and T4-P not implemented, no watchdog armed, no new MMIO write, the ten hashes unchanged, `boot` 0, feature mask 0, T3 and T4 not waived, the first XNU run on HOLD.
