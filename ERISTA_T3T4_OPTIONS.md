# Ways to the first XNU boot: T3 and T4, one only, or neither

Offline comparison of 2026-11-27, with option D added the same day. The goal of the first run is the entry into XNU and VM initialisation (the VM-stop diagnostic), not a booted iPadOS. Nothing here is built, approved or waived: the T3/T4 waiver is undecided, `boot` is 0, the feature mask is 0, no new MMIO is approved, and every run stays on HOLD. Sizes come from `ERISTA_T3T4_MINIMAL.md`, `ERISTA_T3T4_REUSE.md` and `ERISTA_T2_RESULT.md`; they count code and runs, not days.

## What no option removes

Rows 5, 6, 8 and 9 of `ERISTA_FIRST_XNU_BLOCKERS.md` apply to all three: the stop path after XNU owns the core is unproven, the written seed decision is open, the boot=1 build under test is not approved, and marks that trickle in are not bounded by the stop rules. Nothing in A, B or C changes that.

## The options

| | A: T3 and T4, then XNU | B: T3 only, T4 deferred | C: external cooling and a strict manual stop |
| --- | --- | --- | --- |
| New loader code | the fan step (about 800 lines with its audit and model tests, on the scale of T2), an I2C5 driver and a PMIC watchdog step, and the fan and the watchdog carried into the first-XNU loader | the fan step, carried into the first-XNU loader | none |
| New MMIO | pinmux, GPIO banks A, CC, S, V, PMC bits, PWM; I2C5 register writes (never done on this console) | the first group | none |
| Hardware runs before XNU | at least four: PWM and 5 V state read, fan on with edges counted, PMIC identity read, watchdog arm and clear, and the restart behaviour (five if the state read is its own run) | one or two: the state read and the fan run | none |
| Autonomous stop for a hang | a PMIC watchdog, if its restart behaviour is acceptable | none | none |
| Fan during the run | on, if the fan survives XNU's early init, which no source shows | on, same condition | off, as every loader launch leaves it; air from outside the console |
| Who stops a hang | the watchdog, then the person | the person | the person |

## Risks that remain, by option

**A.** It is the only option with a hardware stop for a hang, and every piece is new:
- Whether the MAX77620 watchdog stays enabled across the restart it causes is open (`ERISTA_T3T4_MINIMAL.md`). If it does, hekate restarts every period.
- A disarm before `SYSTEM_RESET` needs an I2C5 write from the kernel side, or a period longer than the run, which is the time limit the owner held back.
- The 5 V rail carries USB VBUS handling (CC4); a wrong write acts on more than the fan.
- Its period has no measured basis (no heat or mark timing from XNU exists).
- It is not an overheat protection; SOC_THERM is a separate driver (`ERISTA_T3T4_REUSE.md`).

**B.** It starts the fan and does nothing for a hang:
- The fan runs only if XNU's early init leaves the pinmux, GPIO and PWM alone. Not shown.
- Fan edges are counted by a polled GPIO; a fast fan loses edges and the count reads low.
- A hang before VM done has no autonomous stop, as in C. The run is seconds to minutes, so what the fan adds over C is small and unmeasured: D1-min's one observation is a 4.1 C rise across hekate, Nyx and a 15 s dwell.
- It does add the PWM and 5 V writes, with the USB-rail risk of A.

**C.** It needs nothing new, so every protection is the person's:
- A hang with the fan off has no autonomous stop. The person holds POWER; how fast the person acts and how quickly the SoC heats are not measured.
- External air cools the case and the heatsink path to the degree the airflow reaches them. How much it cools the SoC is not known and not measurable here.
- The stop rules are the existing ones (30 s with no new mark, `n + 10` s after the VM text, start below the provisional 40 C). They are not official limits, and the 10 s is a guess.
- Marks that trickle in make the longest run about 10.5 minutes by arithmetic. That is not a safety limit; row 9 holds it, and `ERISTA_FIRST_XNU_RUN.md` lists the three changes the owner has not chosen.

## What cannot be guaranteed

- That XNU's early init does not heat the SoC faster than the person, or a watchdog, reacts.
- That the PSCI `SYSTEM_RESET` works after XNU owns the core (row 5).
- That a fan started by the loader is still running when the kernel is.
- That a time limit is a safety limit: none of the three options turns one into a guarantee.
- That any temperature seen at the loader (T2's 34.3125 C) says anything about XNU's early heat.

## Reading

A is the largest and the only one that adds a hardware stop, and its watchdog has an open question that could only be settled by its own runs. B adds the second-largest amount of new MMIO for a benefit that is small for a run this short and does not cover the hang. C adds nothing and is exactly the condition `ERISTA_FIRST_XNU_BLOCKERS.md` already states: a person at the console as the only mechanism. Choosing between them, or waiving T3 and T4, is the owner's decision in writing; this document recommends none.

## D: T4 first, with a SoC watchdog and not the PMIC

T4 is three separate things (`ERISTA_T3T4_REUSE.md`): a watchdog against a hang, a thermal trip, and which watchdog. D means the first one only, and it asks whether the MAX77620 can be left out.

**Can T3 and T4 be built independently?** Technically yes. T3 touches pinmux, GPIO, PMC power bits and PWM; a TKE watchdog touches the timer block at `0x60005000` and the PMC2CAR reset; the PMIC watchdog touches I2C5. No register or driver is shared. The only tie is a policy one in `ERISTA_THERMAL_DESIGN.md`: a reset should land where the fan is known. After any restart that is hekate and Nyx, and the fan is off at every launch today, so D gives the same fan state as every current run. Neither is a prerequisite of the other.

**Watchdogs other than the MAX77620.**

| Candidate | What the repository and the sources show | What is not known |
| --- | --- | --- |
| WDT4 on TMR6 with PMC2CAR reset | `kernel/switch/diag/heartbeat.c` (debug kernels) arms it: unlock `0xc45a`, disable, `TMR6_PTV`, config `6 \| (48 << 4) \| (1 << 12) \| (1 << 15)`, start; the comment says no FIQ and no core reset, because both go to the BPMP. Its warm reset follows 47.75 to 48 s after the last beat. The same block is hekate's `watchdog_start` (TMR9, FIQ only, ended before launch) | Whether it was ever observed to reset an Erista (no result in the files read); that the same PMC reset lands in hekate as `SYSTEM_RESET` does (the same destination is likely, not shown); `timer.h`'s bit definitions were not read |
| WDT0 on TMR5 with PMC `MAIN_RST` | the panic and shutdown paths arm it after 45 to 60 s (`SwitchPanicSave.c`, `shutdown.c`); `ERISTA_STOP_PATHS.md` records that hekate saw RAM intact after that path | the same: no run on an Erista is recorded here |
| MAX77620 watchdog | needs an I2C5 driver nothing has ever used on this console; whether it stays armed across the restart it causes is unknown | the boot-loop question of `ERISTA_T3T4_MINIMAL.md` |

Both TKE watchdogs have code in this repository to port (GPL-2.0, the same repo), so D needs no PMIC driver. Neither is armed by any loader: the kernel arms WDT4 when `watchdogd` connects, which the first run never reaches, since it ends at VM done.

**A stop that is independent of the BPMP.** None is available. The FIQ and the core reset of a TKE watchdog go to the BPMP, and what the BPMP firmware does with them is a closed binary this repository does not hold. The PMC2CAR reset does not need a kernel thread, but whether it needs a live BPMP firmware is not shown.

**After the hand-off to XNU.** A watchdog armed by the loader just before the jump keeps counting while XNU runs, if XNU's early init (before IOKit) does not touch the timer block. The kernel's own heartbeat is not armed by then, so the two do not meet in the first run. Not shown: that nothing in the early path writes those registers.

**What it is, and is not.** An armed, unpetted watchdog is a hardware time limit on the whole run, whatever the cause: a hang, a panic that leaves the screen up, or trickling marks. It is the first of the three held options in `ERISTA_FIRST_XNU_RUN.md` (a total time limit) enforced by hardware. It is not a temperature protection, and its period has no measured basis: too short resets a healthy run, too long protects nothing. A time limit is still not a safety limit.

**Restart loops.** Three things stand between D and a loop, none proven:
- the watchdog sits in the SoC's timer block, which a full SoC reset returns to its reset state, unlike a PMIC whose state may outlive the SoC restart (architecture reasoning, to be read back, not measured);
- the package arms nothing in hekate, and the card's entry is not hekate's autoboot;
- hekate's own `watchdog_fired()` path skips its LP0 and Minerva step when the status is set, and the code read shows no `watchdog_end` on that path, which is where a leftover armed state would show.

If the timer block kept its state across the reset, hekate's own TMR9 use could collide with it. That needs a read-only look at the registers after a restart, and any new read is not approved.

**What D needs.**
- New code: a loader step of about the size of five register writes plus the same reads back, built only into the debug first-XNU loader (nothing in release), and its audit and model tests. No kernel code if the run relies on the reset clearing the timer; a debug kernel disarm if not.
- New MMIO: reads and writes in the TKE block (`0x60005000`: `TMR6_PTV` `0x68`, `WDT4_CFG` `0x180`, status `0x184`, command `0x188`, unlock `0x18c`, shared status `0x1a0`) and the PMC reset state. T1 and T2 touched only CAR reads and I2C1, so all of it is new allowlist.
- Hardware runs before any XNU run, at least three: a read-only dump of those registers at the loader; arm with the loader parked and let it expire, to see the reset land in hekate and read the state after; arm and reset by `SYSTEM_RESET` to show the timer does not survive it. Then the XNU run itself.

## Comparison with the other options

| | A | B | C | D |
| --- | --- | --- | --- | --- |
| Fan during the run | on, unproven | on, unproven | off, outside air | off |
| Hardware stop for a hang | PMIC watchdog | none | none | TKE watchdog |
| New code | fan, I2C5 driver, PMIC step | fan | none | a few register writes, with a port from this repository |
| New MMIO groups | pinmux, GPIO, PMC, PWM, I2C5 | the first four | none | timer block, PMC reset state |
| Runs before XNU | four or more | one or two | none | three or more |
| Open question that can sink it | PMIC state across the restart | the fan surviving XNU's early init | the person's reaction time | which reset it gives, and BPMP's part in it |

D is smaller than A and has code to start from, and its open question (what the TKE watchdog's reset does on this console) is one that a run before XNU can answer. It leaves the fan off, so it bounds time, not heat. None of the four is recommended here, and none of them lowers a hold.
