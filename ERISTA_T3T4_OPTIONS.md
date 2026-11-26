# Three ways to the first XNU boot: T3 and T4, one only, or neither

Offline comparison of 2026-11-27. The goal of the first run is the entry into XNU and VM initialisation (the VM-stop diagnostic), not a booted iPadOS. Nothing here is built, approved or waived: the T3/T4 waiver is undecided, `boot` is 0, the feature mask is 0, no new MMIO is approved, and every run stays on HOLD. Sizes come from `ERISTA_T3T4_MINIMAL.md`, `ERISTA_T3T4_REUSE.md` and `ERISTA_T2_RESULT.md`; they count code and runs, not days.

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
