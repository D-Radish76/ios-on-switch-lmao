# T4-R: source review of 2026-11-27

Offline read of `boot/diag/loader/t4r*.c`, `t4r.h` and the `diag.c` block at `d3aea29e7`, after the Mac build. Nothing was changed by this review, no code, no allowlist, no image.

## What the review confirmed

- Four addresses in one table (`t4r_regs.c`), one gate (`t4r_gate.c`) that checks the allowlist before it uses the address, one word load (`t4r_access.c`). The Mac audit agrees: each address once in the image, one caller.
- Each read is between a drawn BEGIN line and a drawn RESULT line, and a line that does not settle ends the run before the next read.
- No store to a device address, no PMC, no CAR, no command or unlock register (`0x108`, `0x10c`, `0x188`, `0x18c`), no `smc` or `wfe` in the T4-R files. The return path is the shared exit.
- The state is only printed. Nothing branches into a register access because of it.

## The status bit layout, rechecked

The classification took status bit 0 as enabled and bit 1 as an interrupt, from the heartbeat code for WDT4, and called every other status bit unnamed. Rechecked against Linux `tegra_wdt.c` (fetched 2026-11-27):

| Status bits | Source | Weight |
| --- | --- | --- |
| 4 to 11 | the current count, in quarter periods (`WDT_STS_COUNT_SHIFT 4`, mask `0xff`) | Linux, mainline, for WDT0 |
| 12 to 13 | the expiry count (`WDT_STS_EXP_SHIFT 12`, mask `0x3`); the reset fires on the fourth | Linux, mainline, for WDT0 |
| 0 (enabled), 1 (interrupt) | `kernel/switch/diag/heartbeat.h`, for WDT4; Linux does not define them | this tree only, never measured on this console |

Two consequences of the first build, neither a risk, and the first now corrected:

1. A running WDT0 has a nonzero count in bits 4 to 11. The current code reports that as UNKNOWN with the reason "status bits no source names". That text is wrong for bits 4 to 13: a source does name them. The state is still the safe one (UNKNOWN, nothing touched), but a running watchdog would be shown as UNKNOWN and not BUSY unless the status had no such bits.
2. Bits 0 and 1 are an assumption twice over: that WDT0's status has the layout the heartbeat code uses for WDT4, and that the heartbeat's bit meanings are right. The screen says UNKNOWN for what it cannot place and prints the raw words, so the assumption cannot hide a value. It can only mislabel one.

Made on 2026-11-27 at the owner's approval (the T4-R hash may change within this scope): bits 4 to 11 and 12 to 13 are named in `t4r.h`; a running timer (`TMR5_PTV` bit 31 with a value) is BUSY; a count or an expiry count left on a stopped timer is UNKNOWN with its own reason (count, expiry, or both); an enabled timer with no value is UNKNOWN (conflict); any status bit no source defines (2, 3, 14 to 31) is UNKNOWN; the count and expiry have their own screen line, which also says that bits 0 and 1 are not confirmed. The state is still only drawn: no extra register access, the same four registers, the same five reads, and the image still has no store. Flat hash `de467deb...` (Linux); the Mac build is to be repeated.

## Limits that stay

- The WDT0 registers may not answer at EL1 here, and a read that hangs the bus cannot be left by software (`ERISTA_T4R_RUN.md`).
- `TMR5_PTV`, the config word and the status layout come from mainline Linux and this tree's panic path. L4T's DTB names timer index 7 and `0x60005088` for WDT0; T4-R does not read them, so that disagreement is not settled by this stage.
- The state is the state after hekate's init and after hekate launched the loader. It is not the state right after a reset.
- `TMRUS` counting and a return to hekate do not show a watchdog reset.
