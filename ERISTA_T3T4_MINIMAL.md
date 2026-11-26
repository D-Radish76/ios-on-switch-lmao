# T3 and T4 in their smallest form: offline feasibility

Review of 2026-11-27, from `ERISTA_THERMAL_DESIGN.md`, `ERISTA_THERMAL.md` and the diagnostic loader's sources. Nothing is built, approved or waived. T3 and T4 stay on HOLD, the T3/T4 waiver is
undecided, and the MMIO allowlist is not changed by this document.

## T3: the smallest fan step

Smallest form: the design's own T3 (fan on at the lowest duty, edges counted), without any later step. It cannot be smaller and still prove anything: starting the fan without counting edges would be the
same register writes with no evidence that it turned.

| Need | Detail | Status |
| --- | --- | --- |
| New register blocks in the allowlist | pinmux (`SATA_LED_ACTIVE`, `USB_VBUS_EN0`, `LCD_GPIO2`, `CAM1_PWDN`), GPIO banks A, CC, S, V, PMC (`NO_IOPOWER`, `PWR_DET_VAL`), PWM `PWM_CSR_1`. T1 and T2 touched only CAR reads and I2C1 | none of it exists in the loader |
| A state read first | PWM clock and reset bits, the same registers' current values. If the PWM clock is off the step stops: turning it on is a CAR write the design leaves out | design only |
| The 5 V rail | on the Erista it also carries USB VBUS handling (CC4). A wrong write acts on a rail shared with USB, not only the fan | unreviewed against hardware |
| Tachometer | a polled GPIO, so a fast fan loses edges and the count reads low. No expected rpm is known | design only |
| What the step proves | that this console's fan turns when the loader asks. Not that it keeps the SoC cool, and not what XNU's early init does to those pins | |
| Cost | a loader step on the scale of T2 (about 800 lines in `t2*` files and tests), its audit and model tests, a Mac build and one attended run | |

What T3 would give the first XNU run, if it passed and the fan were left running: a fan that is on while the kernel runs. Not shown: that XNU's early init leaves the pinmux, GPIO and PWM alone before the VM
text, that the fan survives the PSCI reset, and what hekate does with it afterwards (Nyx's launch sets duty 0, so hekate and Nyx return the console to a known state).
Leaving the fan on is a separate decision the design leaves to the owner; the off sequence is the one with a hekate precedent.

## T4: the smallest protection

Two parts in the design; they protect against different things.

| Part | Smallest form | Feasibility |
| --- | --- | --- |
| Watchdog against a hang | the MAX77620 watchdog (I2C5, address `0x3C`; `CNFGGLBL2`, `CNFGGLBL3`, `ONOFFCNFG2`): arm with the longest period that fits, clear it on every exit path | possible only with a driver for I2C5 writes (T1-T read I2C5's controller registers only; no PMIC register has been read or written) and a kernel-side disarm before `SYSTEM_RESET`. **Open:** whether the PMIC keeps the watchdog enabled across the restart it causes. If it does, hekate is restarted every period. This is not shown anywhere read, and a test of it is itself a watchdog run |
| Overheat trip | SOC_THERM thermtrip, with the PMC command that makes it act | not minimal: the sensors need their clocks and fuse calibration first ("a driver's worth of work"), the packaged DTB gives no PMC reset command, and what a trip does (restart or power off) is not settled. It cannot be tested by heating the console |
| Tegra watchdog (TKE, WDT4) | the other candidate | the TRM is needed for which timer resets the system from an A57; hekate's own call has no reset bit. Not designable from the sources read |

So the smallest T4 is the PMIC watchdog alone. It bounds a hang with the clock the owner chooses, which is the time limit the owner held back, now enforced by hardware. It is not an overheat protection, and
the choice of its period has the same missing basis as any time limit (no measured heat or mark timing). Its order of work: a read-only PMIC identity read (a new write-free step that still needs I2C5 in the allowlist),
then the arm and clear test, then the restart behaviour, each with its own run.

## Order and what this means for the first run

1. T3 first: T4's reset needs a destination that cools the console, and the design says so (the restart goes to hekate and Nyx, where the fan is known).
2. T4 second, PMIC watchdog only, with the disarm path proven before it is armed in any run that reaches XNU.
3. Neither is a small addition to the first XNU package: each is a design review, new allowlist entries, a Mac build and an attended hardware run, and each can end the work by showing something unexpected.

Nothing here lowers the HOLD. Whether the first run goes ahead without them, with them, or with a waiver is the owner's written decision (`ERISTA_FIRST_XNU_BLOCKERS.md`, row 7). Without them the person's POWER hold is the
only protection, as `ERISTA_FIRST_XNU_RUN.md` says.
