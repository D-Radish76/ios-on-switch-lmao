# T4-R: what it reads, and why that much

Detail design of 2026-11-27 for the read-only stage of `ERISTA_T4_DIAG_PLAN.md`. Nothing here is built or approved. It replaces the register list and the evidence rule of that plan where they differ.

## Findings that change the plan

Read from hekate v6.5.5 and Linux `tegra_wdt.c` (fetched 2026-11-27), not from a measurement.

1. **The timer number.** Mainline Linux drives WDT0 (config `0x60005100`) with timer 5 (`WDT_TIMER_ID 5`, timer registers at `0x60005060`), config word `5 | (timeout << 4) | (1 << 15)` (PMC2CAR reset). That equals the panic path's word and offset. Hekate gives WDT4 at `0x60005180` and timer 9 at `0x60005080`, with bits 12 IRQ, 13 FIQ, 14 SYSRESET, 15 PMCRESET. The DTB's `0x60005088` and index 7 are L4T's node and match neither; `0x60005088` is timer 0 in the heartbeat's table. T4-R does not need that node, so it is left open and costs nothing.
2. **Hekate erases the reset cause.** `hw_init` saves `PMC_RST_STATUS` and `SCRATCH200` into variables, then writes `SCRATCH200` 0 and `PMC_RST_STATUS` to power-on. Every loader launched from hekate therefore reads `PMC_RST_STATUS` 0, whatever ended the last run. The plan's pass rule "`PMC_RST_STATUS` 1 afterwards" cannot be met and is withdrawn. Hekate shows the saved cause only for its own panic codes (`panic_status` and the watchdog reason together).
3. **`watchdog_fired()` is not a hardware read.** It compares the word at `0x4003FF18` (IRAM) with `"WDT"`, which hekate's own exception handler writes. A reset by bit 15 raises no FIQ, so it does not set the word. The plan's remark that an armed leftover makes hekate skip its LP0 step was wrong for this word. What an armed WDT4 does to hekate is different: `watchdog_start` programs WDT4, and config writes are ignored while the counter runs (the heartbeat's comment), so a WDT4 left armed would keep our word, not hekate's FIQ-only word. That is one more reason to prefer WDT0.
4. **The timer's clock and reset are already measured.** `CLK_L_TMR` is bit 5 (hekate `clock.h`). `CLK_OUT_ENB_L` `0x9802D131` has bit 5 set and `RST_DEVICES_L` `0x049102CA` has it clear, read by D0-S v2 at the loader: the TKE block is clocked and out of reset. T4-R needs no CAR read.
5. **WDT0 and WDT4 sit beside hekate's.** Hekate arms only WDT4, only around LP0 and Minerva, and its end sequence leaves WDT4 enabled on a stopped timer. It never touches WDT0 (`hw_deinit` has no WDT or TKE access).

## The smallest read set

All 32-bit reads, `TMRUS` first.

| Tier | Register | Address | Why |
| --- | --- | --- | --- |
| 1 | `TMRUS` | `0x60005010` | already read by the loader; shows the counter runs; the first read, and the stage ends if anything after it fails to return |
| 1 | `TMR5_PTV` | `0x60005060` | is the WDT0 timer running |
| 1 | WDT0 config | `0x60005100` | source, period, bit 15, whether it is configured |
| 1 | WDT0 status | `0x60005104` | expiry count (Linux: bits from 12), pending state |
| 2 | WDT4 config | `0x60005180` | WDT4 is hekate's: dormant is expected, and it decides whether WDT4 stays a fallback |
| 2 | `TMR9_PTV` | `0x60005080` | hekate's timer, expected 0 |

Tier 1 is four reads and serves the WDT0 choice alone. Tier 2 is two more and only matters for the fallback. Dropped from the earlier plan: the other eight timers, WDT4 status, the shared status `0x600051a0`, both CAR words, `PMC_RST_STATUS` (always 0 here). Never read: `0x108`, `0x10c`, `0x188`, `0x18c`. Not read, and not needed to settle the DTB conflict: `0x60005088` and `TMR7_PTV` `0x70`; if the owner wants it settled, they are a third tier of two reads.

Pass classification for tier 1: idle is `TMR5_PTV` 0 and the config word 0 or without bit 15 on a stopped counter; anything else is busy or dormant, is printed and not touched. The stage still ends with the timed `SYSTEM_RESET`.

## Evidence without the reset cause

The reset cause cannot be read after hekate. T4-A and T4-P therefore pass on what a person and the next T4-R can see:

- the time from `ARMED` to the console resetting, from video, against the period;
- hekate's menu appearing, with no panic screen (a panic screen would show hekate's own status);
- the next T4-R: tier 1 idle, `TMRUS` small. `TMRUS` counts from the SoC reset, so a value at T4-R's first read that is about the time since hekate was launched shows the SoC did reset in between, and a large one shows it did not;
- no second reset in the watch window.

A written marker that survives (a PMC scratch word) would give a direct cause, and it needs a PMC write nobody approved. It is not part of this plan; if the owner wants it, it is a separate request with its own register list.

## Implementation

Tier 1 only, built offline on 2026-11-27: `//boot:diag_t4r`, entry `ios-diag-t4r`, hekate name `iOS Diag T4R`, sources `boot/diag/loader/t4r*.c`, build and check commands in `ERISTA_T4R_RUN.md`. It reads `TMRUS`, `TMR5_PTV`, WDT0 config and WDT0 status as 32 bit words, `TMRUS` first and last. Tier 2 (WDT4) and tier 3 are not built. The state classification above is code (`switch_t4r_classify`), with UNKNOWN for what no source names. Not approved to run.

## Open and measured only by T4-R

Whether WDT0's registers answer at non-secure EL1; whether the DTB's index 7 means anything; whether hekate leaves WDT0 alone in practice (a configured word, a running timer). If any tier 1 read fails to return, stages A and P are not run.
