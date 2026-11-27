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

The Switchroot kernel source (CTCaer `switch-l4t-kernel-4.9`, `drivers/watchdog/tegra_wdt.c` and `tegra-wdt-legacy.c`) was read this round. **Confirmed:** `tegra_wdt.c` uses source 5 and PTV `+0x60` like mainline, and the pinned DTB's `timer@60005000` lists `nvidia,tegra30-timer-wdt` among its compatibles, so that driver can bind there. `tegra-wdt-legacy.c` takes the source from the PTV address it is given; its formula maps PTV `0x88` to source 0, `0x60` to 5 and `0x70` to 7, which agrees with the `heartbeat.h` table. **Unknown:** the `nvidia,tegra-wdt-t21x` driver the DTB names for `watchdog@60005100` (not found in any source reachable here) and so what its `nvidia,timer-index` 7 means; which of these drivers the Switchroot kernel configuration builds (the config was not read).

So the DTB's own pair disagrees with that table, and the table agrees with mainline and with the legacy driver's formula. The vendor documents point at timer 7 and the upstream and this-tree code at timer 5. T4-R read `0x60005060` and found zero, which does not tell which timer the hardware watchdog counts. Only the TRM, or a measurement on a running timer, settles it. A read of the other candidate PTV words would be new MMIO and is not approved. Until it is settled, T4-A's chosen source is a provisional choice, and a wrong source could mean the watchdog counts a timer that is not running (no reset) or one that runs at another rate (a reset at another time). The stop figure has to cover both.

## Write order

Three orders exist (table above). The design proposes the shortest one, the mainline and panic path order, because T4-A starts only from a state read as idle and so needs no preliminary DISABLE:

1. `TMR5_PTV` `0x60005060` = one periodic word;
2. config `0x60005100` = `5 | (P << 4) | (1 << 15)`;
3. command `0x60005108` = 1 (START).

Stop, only on a failed read back, in this order: unlock `0x6000510c` = `0xc45a`, command `0x60005108` = 2 (DISABLE), `TMR5_PTV` = 0. The heartbeat's longer order (unlock and DISABLE first) and hekate's order (unlock first, its stop with a START to clear interrupts) are noted and not used: each adds writes, and a write is a place to hang. If the owner prefers the longer order, the gate changes; nothing else does.

Unknown: whether config is ignored while the counter runs (the heartbeat's comment, from Linux), so an interrupted arm leaves a state only a read back shows.

## PMC2CAR scope and the BPMP

Unknown. The only sources for bit 15 are its name, the legacy driver's names for config bits 12 to 14 (interrupt, FIQ and system reset enables) and the code comments in this tree and Linux; the TRM is not here. A config word that sets bit 15 without bit 14 or the FIQ path is what the mainline arm writes; whether `SYS_RST_EN` (bit 14) is needed on this SoC is not known. BL31's own reset does not use it (`MAIN_RST`). The L4T 28.2 page says that expiry resets the CPU, and does not say the SoC. In a loader stage the BPMP firmware is not running (`NOTES.md`: BL31 powers it off on the Erista when hekate hands it an SC7 entry firmware), so a PMC2CAR reset that needed live BPMP software would not happen; that dependence is not known either way.

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

## Stages

| Stage | Content | State |
| --- | --- | --- |
| T4-A0 | host only: the gate state machine, a modelled watchdog block, the arming and stop models, refusal and fault injection tests (`tools/t4a0/`) | approved 2026-11-27 11:20 JST, implemented, Mac Bazel PASS at `e473fcd44` |
| T4-A1 | the diagnostic image, its static audit and its build | not approved, not started |
| T4-A2 | arming and expiry on the console | not approved, not started |

T4-A0 has no build target that links it into anything, never touches a device access function, and does not generate a loader or a package. The timer source in it is a parameter. Sources 5 (PTV `0x60005060`), 7 (`0x60005070`) and 0 (`0x60005088`) are modelled as candidates; none is adopted for the console.

## The gate: order and state, not only values

A value allowlist would accept the right words in the wrong place. The gate is a small state machine over the write targets, in `tools/t4a0/gate.c`. Its only store is one function, which the host test counts.

| State | Meaning | Allowed next | Moves to |
| --- | --- | --- | --- |
| `INIT` | nothing done | the idle check (three reads) | `CHECKED`, or `REFUSED` if any word is not zero or a read fails |
| `CHECKED` | idle, line drawn | write 1, the one periodic word | `PTV_SET` |
| `PTV_SET` | write 1 done | after read back and a drawn line: write 2, the one config word | `CONFIG_SET` |
| `CONFIG_SET` | write 2 done | after read back of config and PTV and a drawn line: write 3 with 1 | `ARMED`, once per run |
| `ARMED` | write 3 issued; verified once config and PTV read back equal | reads and the park. Nothing is written | stays |
| `STOPPING` | inside the stop sequence | its three writes in order | `STOPPED`, `STOP_FAILED` or `HUNG` |
| `STOPPED` | all three stop writes returned without error and PTV read back 0 | reads | terminal |
| `STOP_FAILED` | a stop write or the final read failed; the step is kept | no access | terminal |
| `REFUSED` | refused before any write; nothing to stop | reads | terminal |
| `HUNG` | an access never returned | no access | terminal |

When the stop sequence runs, and when it does not:

- A request outside the table, a failed read back, a faulted write or a faulted read back runs the stop sequence from `PTV_SET`, `CONFIG_SET`, or an `ARMED` whose read back failed. The sequence is exactly three writes in order: unlock `0x6000510c` = `0xc45a`, command `0x60005108` = 2, PTV = 0. It runs once. It ends with a read of PTV that must be 0.
- From `INIT` or `CHECKED` a refusal writes nothing and ends in `REFUSED`.
- In `ARMED` after a good read back, a stray write request is denied: no write, no stop, the state stays. The run does not undo its own arming because of a bug elsewhere; the park continues.
- A fault in a stop write ends in `STOP_FAILED` at that step. The later stop writes are not tried: a write that failed may have changed the block in a way the model cannot see, and every further write is another place to hang.
- `STOPPED` is not the same as "the counter stopped". The command register is not readable, so the gate cannot see it. `ARMED` is likewise only "the START write returned and the read back matched", not proof that the counter runs.
- No write is accepted in `STOPPED`, `STOP_FAILED`, `REFUSED` or `HUNG`. START is issued once per run; the flag is set when the write begins, so a faulted START still counts, and a second gate in the same run starts `REFUSED`.

A write is accepted only if the previous step's line is drawn, its read back equals what was written, and its address, value and width (32 bit) are the one expected. Every WDT4, PMC and CAR address is refused. All design plus the T4-A0 host model; no device code exists.

## How the gate is tested

Walking all 2^32 values per target is not the plan. T4-A0 (`tools/t4a0/gate_test.c`, run by `t4a0_gate_test`) uses classes:

- every state against the six planned writes, foreign addresses, neighbours of the planned addresses at plus and minus 4, widths 8, 16 and 64, a flip of each of the 64 bits of each planned value, and zero, asserting the table above, including that each refusal writes exactly what the table says;
- the right write without a drawn line or without a read back, in each state;
- every terminal state against every planned write, a verify and a read: nothing is accessed;
- START only once, also after a faulted START and across gates;
- fault injection: each access of the nominal run, each of four kinds (bus fault, hang, silently dropped write, corrupted value), and every pair of two faults, checking that writes are the arm order optionally followed by the stop order, at most one START, a hang ends all access, a refusal before the first write writes nothing;
- a fixed-seed random request sequence from every state.

What it does not show: that the real block behaves like the model (the model is this document's reading of Linux and hekate), that a write at EL1 is accepted, or anything about timing. The model's "counting" is a rule of the model.

## T4-A0 Mac result and review (2026-11-27)

Mac, commit `e473fcd44`: `t4a0_gate_test` Bazel 1 of 1 PASS (0.9 s), its 9 Python tests PASS (0.640 s), build completed. Earlier Mac runs of the same test failed three times on the runfiles tree (`build_data.txt`, the launcher, `_t4a0_gate_test.venv`), never on the C logic. The runfiles entries measured on the Mac are `_t4a0_gate_test_stage2_bootstrap.py`, `_t4a0_gate_test.venv` (directory), `t4a0_gate_test` and `t4a0_gate_test.build_data.txt` (links); `.exe`, `.runfiles_manifest` and `.runfiles` are estimates and are marked so in the test.

Review of the model, found by reading `gate.c`, `sim.c` and `gate_test.c`. Nothing here changed the C code; each item is for T4-A1 or for the next change to the model.

| Finding | Effect | What to do |
| --- | --- | --- |
| Read backs compare the full 32 bit word with what was written | the real block may read back differently (status bits in the config word, a write-only bit, a PTV that returns the running count); the gate would then stop a good arm. That fails safe, but it would end every real run | T4-A1 needs a read back mask taken from a measurement of the real words, not from Linux |
| The stop check reads PTV only and the command register is not readable | `STOPPED` can hold while the counter state is unknown; already stated above | keep the wording; do not treat `STOPPED` as proof |
| `HUNG` keeps no cause and no step | the gate counts writes, the sim logs accesses, but the real image has no such log | T4-A1 should record the access number before each access, in DRAM |
| The idle check reads PTV, config and status; T4-R also read `TMRUS` twice | the model checks less than T4-R did | align when the image is written; the model is not the image |
| At most two injected faults per run, one block, no timing, no expiry, no countdown | multiple independent faults, a slow access and the park loop are not covered | named limits; the park loop is a T4-A1 item |
| Some tests set the gate's own flags (`line_drawn`, `read_back_ok`) to build a case | they are white box tests of the table, not of the public order | the order tests through `t4a_model_run` and the fault injection cover the public path |
| `ARMED` after a faulted START: a bus fault on write 3 runs the stop sequence, and the START flag stays set | one START per run, even if it failed; a retry needs a new run | intended; recorded so nobody removes it |
| The PTV word layout (bits 31 and 30 set, a 29 bit count) and the config word follow Linux and hekate, not a measurement; `T4-R` showed only zeros | the words are placeholders for the gate's shape, not values for the console | T4-A1 conditions 2 and 4 stay open |

## Case temperature and hangs during a run

No temperature source is read by T4-A (the SoC sensor is a new MMIO read and not approved). The run sheet uses an outside measurement: an infrared thermometer or thermal camera at fixed points and times, or a thermocouple taped to the case, with a video that also shows the screen. That is the **case surface temperature**, not the SoC's internal temperature, and the two can differ a lot and by a delay. It gives readings before, at arming, around the expiry window and after, not a continuous record, and an unmeasured peak is stated as unmeasured.

An unmonitorable hang is a real risk: if the SoC stops answering, the screen may stop changing and the person cannot tell a hang from a wait. The countdown comes from the generic timer counter (`cntpct`, a CPU register, no MMIO), and the screen is redrawn from DRAM. A frozen countdown **does not show that the CPU stopped**: the display path, a halted scan-out or a stalled bus can freeze a picture while the CPU runs, and a running countdown does not show that the watchdog is counting. The person's rule is fixed before the run from the video timings; POWER hold is attempted at it and is not counted on (`ERISTA_T4A_RESET_PATH.md`).

## Image and sequence

Not part of T4-A0. For T4-A1: one image, `diag_t4a`, new target, own sources, allowlist and audit, entry `ios-diag-t4a`; the ten diagnostic hashes stay as they are. WDT0 only; WDT4 is not touched. Sequence: title, EL1 and board checks (not an Erista: no register touched); the T4-R read set, each read drawn first; continue only on IDLE with all three words zero; draw `ABOUT TO ARM`; writes 1 to 3 each drawn first with the gate; read back and draw; `ARMED, RESET EXPECTED IN ABOUT <T_A> S` once.

The park after `ARMED` is a loop that reads `cntpct`, redraws the countdown from DRAM when the second changes, and then waits. A bare `wfe` never wakes without an event, so the loop needs one: the generic timer's event stream (`CNTKCTL_EL1.EVNTEN`) wakes `wfe` at a fixed rate without an interrupt controller or MMIO, so it avoids both polling and a register access. Whether EL1 may set it on this console is unknown and is a T4-A1 question; the fallback is a spin on `cntpct`, with no register access and no PSCI call. Either way the park touches no TKE register.

## The period is not taken from existing values

The 45 s, 48 s and 60 s of the shutdown watch, heartbeat and panic path are timeouts chosen for a running kernel. They are not safety limits, none was ever run on an Erista here, and none is the basis of `T_A`. `T_A` is the owner's choice from three things: the time the person needs to read the lines and reach POWER (to be taken from a video of the first run), the longest wait the owner accepts with the console parked, and the unit's meaning, which is not measured: the period field's unit follows Linux's comment (a 250 ms tick, four expiries), and a mistake of that kind scales `T_A`. A time limit is not a safety guarantee.

## Unresolved, with the source behind each

| # | Item | Status | Grounds | What would settle it |
| --- | --- | --- | --- | --- |
| 1 | Where the console lands after a WDT0 expiry | unknown | BL31 and hekate do the return only on the PSCI path (`ERISTA_T4A_RESET_PATH.md`) | the owner's description of a cold start, then an observation |
| 2 | How the owner's console starts hekate from cold (modchip, payload on the boot path, jig and injector) | unknown to this tree | none | the owner |
| 3 | Whether any stage can overwrite `R2P_PAYLOAD_BASE` (`0xFFE40000`, secure firmware carveout) | base confirmed, overwrite risk inferred low | hekate `l4t.c`; the loaders do not touch it | the T4-A audit lists no store there |
| 4 | Timer wiring: source 5 at `0x60`, or 7 at `0x88` | unknown | mainline and this tree against the DTB and L4T 28.2; the `heartbeat.h` table agrees with neither DTB pair | the TRM; or a read-only extension of T4-R (needs approval) |
| 5 | Whether mainline's driver binds on T210 | unknown | mainline lists `tegra210-timer` only; the pinned DTB adds `tegra30-timer-wdt` | the Switchroot kernel config |
| 6 | The `nvidia,tegra-wdt-t21x` driver, its write order and the meaning of `nvidia,timer-index` 7 | unread | the DTB names it; not found in any reachable source (GitLab blocked) | the NVIDIA kernel source |
| 7 | Writes at EL1 | unknown | reads answered; BL31 never touches the block | T4-A itself |
| 8 | Status bits 0 and 1 for WDT0; the expiry count | unknown | none; the zero word names nothing | the read back after arming |
| 9 | The scope of the PMC2CAR reset (CCPLEX or SoC) and the BPMP's part | unknown | the L4T 28.2 page speaks of resetting the CPU; BL31's reset is `MAIN_RST` | the TRM |
| 10 | Survival of the TKE setting across a reset | unknown | see above | the read after a reset |
| 11 | A boot ROM watchdog (`WDT_DURING_BR`) | unknown | hekate's `hw_init` comment | the TRM |
| 12 | The unit of the period | inferred | Linux comment: 250 ms tick, four expiries; not measured | the video of T4-A |
| 13 | The temperature during a run, and a hang that cannot be seen | unmeasured | only before and after were taken | external readings on video (above); a countdown from `cntpct` |
| 14 | PSCI after an armed watchdog (T4-P) | out of scope here | `ERISTA_T4_DIAG_PLAN.md` | not designed here |

## Conditions before T4-A could be implemented

These open T4-A1 (the image). T4-A0 needed none beyond the approval given. All must hold and be recorded.

1. The T4-A0 model, its tests and its isolation checks pass on the Mac as well as here.
2. Items 4 and 6 settled from the TRM or the NVIDIA kernel source, or the owner accepts a provisional timer source in writing.
3. The write order chosen from the comparison above: three arming writes and three stop writes.
4. `T_A` and the stop rule set by the owner by the rule above, with the video timings of the first run in hand.
5. The ten earlier hashes unchanged in the build that adds `diag_t4a`.
6. The owner's written approval to start T4-A1.

The static audit of the image (every store listed once, none outside the gate) is a T4-A1 deliverable, not a condition for starting it.

## Conditions before a real device run

These open T4-A2. Each needs the T4-A1 image built on the Mac and recorded.

1. The image's hashes recorded, the card compared before and after placement, and the static audit passing.
2. A run sheet like `ERISTA_T4R_RUN.md`: stop conditions, case temperature readings during the run, the video setup, the card hash before and after.
3. The owner's recovery rehearsal without any image of ours: time a POWER hold from a running hekate, enter RCM with the jig from off, inject hekate and note the time from injection to the menu. This shows what works from a calm state and says nothing about a state with a watchdog armed.
4. The owner's separate written approval of the three arming writes, the three stop writes, the recovery being unverified for a watchdog left armed, and the loop risk.
5. The owner's answer to what happens if the console lands outside hekate (item 1) and agreement to inject hekate again.

Standing limits: T4-R not run again, T4-A and T4-P not implemented, no watchdog armed, no new MMIO write, the ten hashes unchanged, `boot` 0, feature mask 0, T3 and T4 not waived, the first XNU run on HOLD.
