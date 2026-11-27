# T4 automatic recovery: where it stands, and the next steps

Research and design of 2026-11-28, documents only. No code changed, no new MMIO operation implemented, nothing run, nothing copied to a card, no watchdog armed. `boot` 0, feature mask 0, T3 and T4 not waived, the first XNU run on HOLD. The T3 real device run is on HOLD by the owner (2026-11-28 02:33 JST) because recovery from an MMIO hang with the 5 V rail on is not established. This file reuses and does not repeat `ERISTA_T4_WATCHDOG_DESIGN.md`, `ERISTA_T4A_DESIGN.md`, `ERISTA_T4A_RESET_PATH.md`, `ERISTA_T4_DIAG_PLAN.md`, `ERISTA_T4R_RESULT.md`, `ERISTA_T3T4_MINIMAL.md` and `ERISTA_FIRST_XNU_REMAINING.md`.

Marks as in `ERISTA_T3_SAFETY_RESEARCH.md`: P primary, S secondary, I inference, U unknown or not reachable, M measured here. Pages were read through a fetch tool that summarises; a "tool summary" is a paraphrase.

## T4-A0, checked again

| Check | Result |
| --- | --- |
| `tools/t4a0/gate_test.c`, built by hand with clang 18.1.3 (`gate.c`, `sequence.c`, `sim.c`, `gate_test.c`) | `t4a0 gate: 139757 checks, 0 failures`; fault injection 11 accesses by 4 kinds and all pairs: no write outside the arm order and the stop order, one START at most, a hang ends all access |
| `t4a0_gate_test.py` | 9 tests OK |
| Mac record, `e473fcd44` | Bazel 1 of 1 PASS, Python 9 PASS (`ERISTA_T4A_DESIGN.md`); not repeated for a later commit |
| Code changed since `e473fcd44` in `tools/t4a0` | none (`git log`) |

It shows that the gate's order rules and its refusals hold against the model in this tree. It does not show that the real TKE block behaves like the model, that a write at EL1 is accepted, anything about timing, or anything about a bus hang. The model's "counting" is a rule of the model.

## What T4-A1, T4-A2 and T4-P still lack

Status after this round. "Closer" means the new evidence below narrows it; nothing is settled by a source alone.

| Item | Needed for | Status |
| --- | --- | --- |
| Read back masks for config and PTV taken from a measurement, not from Linux | A1 | open. T4-R read only zeros; the first read after arming is the first sample |
| Access number recorded before each access, in DRAM | A1 | open, a loader change |
| Idle check aligned with T4-R (two `TMRUS` reads) | A1 | open |
| The park loop (`wfe` with the event stream, or a `cntpct` spin) | A1 | open; the event stream wrote `CNTKCTL_EL1` in the loader exit before, so it is plausible at EL1 |
| Timer source: index 5 at PTV `0x60005060`, or index 7 | A1 condition 2 | open. NVIDIA's L4T 28.2 page says "WDT0 is configured to use TIMER7" (P, vendor, tool summary); mainline Linux and this tree's panic path use 5. Only the TRM or a measurement settles it |
| The period unit and `T_A` | A1 condition 4 | open; Linux's 250 ms tick and four expiries are comments, unmeasured |
| Where the console lands after a WDT0 reset | A2, owner's answer | **open, see the paths below** |
| What the owner's console does at a cold start with no jig | A2 | unanswered; the owner knows it |
| The loop outcome (the TKE block keeps its configuration and count) | A2, P | unknown; only a run after a reset shows it |
| The reset after PSCI `SYSTEM_RESET` with a watchdog armed | P | design only (`ERISTA_T4_DIAG_PLAN.md`) |
| The A5 and CC4 pins after any reset | T3 | unknown, and not measurable with our tools (see T3 below) |

## An independent recovery path: WDT0 on TMR5

The sequence, the word, the gate and the stop order are in `ERISTA_T4A_DESIGN.md`. New here is what the sources say about it firing while the CPU is stuck.

### What the evidence says

| Source | Statement | Mark |
| --- | --- | --- |
| Linux `drivers/watchdog/tegra_wdt.c` (master), `https://raw.githubusercontent.com/torvalds/linux/master/drivers/watchdog/tegra_wdt.c` | start: `TIMER_PTV` with `1000000/4 \| EN \| PERIODIC`, `WDT_CFG` with the timer id, timeout shifted by 4 and `WDT_CFG_PMC2CAR_RST_EN` (bit 15), then `WDT_CMD` start. Stop: unlock, command disable, PTV 0. Comment: "the watchdog system reset actually occurs on the 4th expiration". `INT_EN` is bit 12. The only compatible is `nvidia,tegra30-timer` | P (tool summary) |
| NVIDIA L4T 28.2 "Watchdog Timer", `https://docs.nvidia.com/jetson/archives/l4t-archived/l4t-282/Tegra%20Linux%20Driver%20Package%20Development%20Guide/WDT_proc.html` | WDT0 uses TIMER7 on L4T; default timeout 120 s; "the WDT0 hardware sends a reset signal to the CPU that causes it to reset"; "if an application terminates or hangs, a Watchdog timer eventually expires". It does not say whether a hung CPU is covered, and it speaks of a CPU reset, not a SoC reset | P (tool summary) |
| NVIDIA developer forum, "How can I generate a hard reset with WDT?" (Jetson TX1, Aug 2018), `https://forums.developer.nvidia.com/t/how-can-i-generate-a-hard-reset-with-wdt/63716` | users report that with only the PMC2CAR enable the restart happened at the 4th expiry but boot then stopped after "Starting CPU & Halting coprocesser", cboot locked up after some hundreds of tests, and a responder (staff status not stated) called it a known issue and pointed to a TX1 erratum "BR-2" that is not described in the thread | S, and the only T210 report of a WDT restart found |
| NVIDIA L4T 28.2 "Secure PMC Scratch Register Configurations for BootROM", `https://docs.nvidia.com/jetson/archives/l4t-archived/l4t-282/Tegra%20Linux%20Driver%20Package%20Development%20Guide/secure_src.html` | a reset table lists "Fourth WDT expiration" at level L2 preserving DRAM and internal RAM, and "Fifth WDT" at L1. It names TX1 and TX2. The labels may mean expiry counts or watchdog instances; the tool summary cannot tell | P, meaning unclear |
| This tree: D0-S v1 hung with no hardware reset and was recovered by POWER hold and RCM (`ERISTA_D0S_RUN.md`: "a hang has no hardware reset"). Which instruction hung was never identified (`ERISTA_D0S_V2_RESULT.md`) | no known address to hang on purpose | M |

No source found states that a TKE watchdog resets the SoC while a bus access is stuck. The TRM is not reachable here.

### Conditions for the watchdog to act during an MMIO hang

All of these are inference from how a free running timer and a hardwired reset signal are normally built. None is shown for T210 by a file read here.

| Condition | Why it is needed | Mark |
| --- | --- | --- |
| The TKE timer clock keeps running | the count does not need the CPU or the APB bus after the arm, only its clock | I |
| The arm writes returned and read back as expected | an arm that hung half way leaves no counting watchdog (`ERISTA_T4A_DESIGN.md`, "A hang at each step") | design rule, untested on hardware |
| Bit 15 (`PMC2CAR_RST_EN`) set, and the PMC acts on it | the reset path to the CAR is the thing that does not need the stuck master | I; PMC conditions U |
| The stuck access does not hold the PMC or CAR reset logic | a hang that stalls only the CPU's bus master leaves them alone; a power or clock failure may not | I |
| BPMP firmware not required | the BPMP is off in a loader stage (`NOTES.md`); whether the PMC2CAR reset needs it is unknown | U |
| The period is long enough to read the screen and short enough to matter | `T_A` is the owner's choice and not a safety limit | design |
| The hang is a stalled bus access, not a whole system stall such as a DRAM or EMC fault | a whole system stall may stop the reset path too | I |

So the watchdog could end a hang, but a run is the only way to find out, and the one hang type that matters (a stuck access) cannot be produced on purpose without a new, hang-prone MMIO read. T4-A as designed arms and parks the CPU, which tests the reset path with a CPU that is idle, not stuck.

## After the reset: four paths

| Path | Trigger | Where it lands | Evidence |
| --- | --- | --- | --- |
| P1 | loader's PSCI `SYSTEM_RESET` (BL31) | hekate | M: seven runs and T4-R. BL31 prepares the payload copy and the PMC scratch words (`ERISTA_T4A_RESET_PATH.md`) |
| P2 | WDT0 expiry, nothing else prepared | the boot ROM's cold boot: the eMMC chain, or RCM if its conditions hold. Not hekate | I. BL31 is not involved and the reboot-to-payload steps do not run |
| P3 | WDT0 expiry after the loader requested recovery mode in `PMC_SCRATCH0` | RCM, where the owner injects hekate from the Mac | I from three sources below; never done on this console by our code |
| P4 | POWER hold of 12 s or more, then RCM with the jig | RCM, then hekate by injection | the owner's rehearsals: normal hekate only, nothing with the rail on |

P3 evidence:
- `tegra_pmc_writel(2, PMC_SCRATCH0)` then `reset_cpu(0)` is U-Boot's `enterrcm` command, help text "reset Tegra and enter USB Recovery Mode" (`https://git.openwrt.org/project/bcm63xx/u-boot/plain/arch/arm/mach-tegra/cmd_enterrcm.c`, tool summary, an OpenWrt mirror of the tree) (P).
- NVIDIA's public boot flow note says that if PMC scratch0 bit 2 is set at power-up the boot ROM enters recovery mode and that the bit survives Tegra resets, so software can set it and reboot (`https://http.download.nvidia.com/tegra-public-appnotes/tegra-boot-flow.html`, tool summary, generation not stated). The value U-Boot writes is 2, which is bit 1 counted from zero; the note's "bit 2" is probably counted from one (I).
- The pinned BL31 tests bit 1 of the same register and hekate names it `PMC_SCRATCH0_MODE_RCM` (`ERISTA_T4A_RESET_PATH.md`) (P, binary and source).
- Same note: a recovery strap forces recovery mode unconditionally, which is what the owner's jig does (P, tool summary).

What P3 costs and risks:
1. It is one more MMIO write, to the PMC. It is new, not approved, and not in any allowlist.
2. BL31's PSCI reset skips the payload step when that bit is set, so a normal PSCI stop in the same run would land in RCM, not hekate. The bit would have to be set at arm time and cleared with the disarm before the normal stop, which is one more write on the normal path.
3. Who clears the bit after the boot ROM has entered RCM is not shown. If it stays set, every later reset enters RCM until a full power loss clears it (I). The owner would see it as hekate no longer coming back from a PSCI stop.
4. P2 is acceptable only if the owner's console starts something the owner can interrupt. That answer is missing (see questions).

NVIDIA's forum thread is the reason none of these paths is called reliable: a T210 watchdog restart that stalls in boot would need POWER hold, which is P4.

## Alternatives to compare, using existing results

| | SoC WDT0 (TMR5) | MAX77620 watchdog | POWER hold |
| --- | --- | --- | --- |
| Needs feeding | no, a long period is enough | yes, a CPU task must write `CNFGGLBL3` WDTC periodically | no |
| Fits the XNU run | yes, armed once before the jump | no: nothing feeds it under XNU, and a polling loop in the loader is against the project's rule | the person |
| Independent of the SoC bus | partly: needs the SoC's timer, PMC and CAR | more: the PMIC counts and restarts alone (I) | yes (I, shown only in normal hekate) |
| What it does on expiry | resets the SoC, unknown destination | Linux sets `WD_RST_WK` with the comment "WDT expire results in a restart", and the watchdog auto clears in OFF and sleep (`WDTSLPC`, `WDTOFFC`) (`https://raw.githubusercontent.com/torvalds/linux/master/drivers/watchdog/max77620_wdt.c`, tool summary) (P) | cuts power |
| Open | the items above | whether `WDTEN` stays set across the restart it causes (`ERISTA_T1_REVIEW.md`, `ERISTA_FIRST_XNU_REMAINING.md` A4); needs I2C5 writes, never done on this console | the 12 s hold with the rail on |
| Status | candidate | rejected for now in `ERISTA_FIRST_XNU_BOOT.md` for a possible loop; periods 2, 16, 64, 128 s only | the only current recovery |

No new PMIC work is proposed. It stays the fallback if the SoC watchdog is shown not to act during a stuck access.

A different way to learn the unknowns without new loader code: Switchroot's Linux builds `TEGRA21X_WATCHDOG` and `MAX77620_WATCHDOG` (`ERISTA_FIRST_XNU_REMAINING.md` A1) and the pinned DTB leaves `watchdog@60005100` disabled. An owner run of Linux with that node enabled would show, with a driver that exists, where the console lands after an expiry, the period unit, and whether the block persists. It is an arming of a watchdog on this console and needs the owner's decision; it is not proposed as done and not started. It would not show a stuck bus access either.

## T3 and the watchdog: whether to combine

Recommendation: keep them separate until the T4 path is proven, then decide.

| Question | Answer |
| --- | --- |
| What a combined T3 would add | arm (3 writes), a request for RCM (1 PMC write, if P3), disarm (3 writes) and the readbacks, on top of T3's six. The writes in one image would go from 6 to about 13, in a loader whose one hazard is a hang with the rail on |
| What it would buy | a hardware bound on how long the SoC stays hung with the fan rail on, if the watchdog acts during a stuck access, which no source shows |
| What it would not buy | the rail's state after the reset. A5 is a GPIO output latch inside the SoC; whether a reset leaves the pin low, high or floating is unknown (`ERISTA_T3_SAFETY_RESEARCH.md` 24) and our tools cannot read it between the reset and hekate (hekate reconfigures the pins on its way up, as T3-R saw). Only a meter at the fan connector shows it |
| What hekate does afterwards | `fan_set_from_temp` at 39 C or below drives A5 low (`ERISTA_T3_SAFETY_RESEARCH.md` 25), but only once hekate runs and Nyx has a reading; a console parked in RCM keeps whatever the pin did |
| Period | the watchdog `T_A` must exceed T3's 15 s rail limit and its 30 s no-progress rule, and it does not replace the 60 s run cap; the unit is unmeasured until a T4-A run |
| Order if combined | state check, arm, rail on, fan, off sequence, readback confirmed, disarm; a disarm failure or a stop leaves the watchdog armed and ends in a reset. Every write still goes through one allowlist gate with the order rules of `tools/t4a0` |
| Hold | T3's real run stays on HOLD by the owner; this table is design only |

The sequence that makes the watchdog useful for T3 is the one that proves it first without a rail: T4-A, then a stuck access test, then T3 with the watchdog. A combined image before that would test two unknowns at once with the rail on.

## Next steps and the checks each needs

Nothing below is approved. Each step needs the owner's written approval, and none starts T3.

| Step | Content | Exit check before the next |
| --- | --- | --- |
| 0 | Owner answers (below) and decides on the Linux watchdog experiment | answers recorded in `NOTES.md` |
| 1 | Host only: extend `tools/t4a0` with the PMC scratch request and its clear (order and read back), the access log, the idle check with `TMRUS`, and the read back masks as parameters. A model of the park loop's outcomes | `t4a0_gate_test` passes on Linux and on the Mac; tests for each new refusal and fault pair; the earlier hashes untouched |
| 2 | T4-A1: the image `diag_t4a` (entry `ios-diag-t4a`), its allowlist, static audit and Mac build, no run. The audit lists every store and shows none outside the gate, none to `R2P_PAYLOAD_BASE` | audit passes; the eleven diagnostic hashes, `diag_t3r` and `diag_t3` included, unchanged against `907ae9923` and `35d04ec55`; card procedure and dry run as for T3 |
| 3 | T4-A2 run 1: arm WDT0 with a long `T_A`, park, no PMC write (P2). The owner watches where it lands and times it with video; hold POWER at `T_A` plus the margin | the landing and the time recorded; no loop; the unit of `T_A` measured |
| 4 | T4-R after the reset, if it lands in hekate: the TKE state (outcomes 1, 2, 3) | the state recorded; a loop outcome ends the series |
| 5 | If P2 lands outside hekate: P3 in one image, one run, set before arm and cleared before a normal stop | RCM reached, hekate injected, bit behaviour recorded |
| 6 | Stuck access test (owner's decision): arm, then one read of an address chosen to hang with the rail off. The address has to be found from the CAR clock and reset words read before (`CLK_OUT_ENB_L` `0x9802D131`, `RST_DEVICES_L` `0x049102CA`, `ERISTA_T3R_RESULT.md`); no known one exists | the reset happens with the CPU stuck, or POWER hold is used and the watchdog is called not proven |
| 7 | T4-P: PSCI reset with the watchdog armed (`ERISTA_T4_DIAG_PLAN.md`) | no loop after the PSCI stop |
| 8 | Only then: decide on a combined T3 image or a first XNU loader that arms the watchdog | owner's decision with steps 3 to 7 recorded |

Safety checks that apply to every step with a write:
- one allowlist gate, values and order fixed, a read back after each write, a stop sequence that is never held back by time;
- each step drawn and settled before it is made, the last one begun visible on the screen;
- the case temperature, a hand on POWER, and the video, as in `ERISTA_T4R_RUN.md`; the console on battery with nothing on USB;
- no claim from a reset that a register was restored, and no use of `PMC_RST_STATUS` (hekate clears it);
- a time limit written as provisional, never as a safety guarantee;
- the release build carries none of the diagnostic code.

## Questions for the owner

1. What does the console do at power-on with no jig and no USB cable: the eMMC firmware, or something else? This decides whether P2 is a usable landing.
2. Is the Linux watchdog experiment acceptable to run, or only the loader route?
3. Is a copy of the T210 TRM available, for the timer index table, the PMC2CAR conditions and the TKE state after a reset?
4. Is a bench measurement of the fan connector (voltage while the console resets) acceptable? It is the only way to see the rail after a reset.

## Not reachable here

The T210 TRM, the TX1 erratum "BR-2", NVIDIA's `tegra-wdt-t21x` driver, any HAC-001 schematic, and Switchroot's device trees. Nothing in this file depends on them being read; the open items in the tables name them.
