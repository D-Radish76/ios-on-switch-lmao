# T4 as a SoC watchdog: offline design (option D)

Design of 2026-11-27 from the code and documents in this repository and from hekate v6.5.5. Nothing is implemented. No watchdog is armed, no new MMIO is approved, the allowlists are unchanged, `boot` is 0, the feature mask is 0, T3 and T4 are not waived, and every run stays on HOLD. A register whose meaning is not in a source named here is marked, and nothing in this design operates on it. The staged diagnostics are in `ERISTA_T4_DIAG_PLAN.md`.

## What it is for

The first target is the entry into XNU and the VM initialisation, not iPadOS. A watchdog armed just before the loader jumps to the kernel and never fed is a hardware time limit on the whole run. It ends a hang, a panic that leaves the screen up, and a stop path that fails (the timed `SYSTEM_RESET` after VM done depends on `wfe` waking under XNU's `cntkctl_el1`, which `ERISTA_PROTECTION.md` lists as unproven). It does not read a temperature, its period has no measured basis, and it does not make a time limit a safety limit.

## WDT0 and WDT4

Both are in the TKE block at `0x60005000`. The register layout is the same, 0x20 apart; the offsets are in `SwitchPanicSave.c` (WDT0) and `kernel/switch/diag/heartbeat.h` (WDT4).

| | WDT0 | WDT4 |
| --- | --- | --- |
| Registers | config `0x100`, status `0x104`, command `0x108`, unlock `0x10c` | config `0x180`, status `0x184`, command `0x188`, unlock `0x18c` |
| Who arms it today | the panic path: source TMR5, word `5 \| (seconds << 4) \| (1 << 15)`, no interrupt, only when a reboot delay is set (the first-XNU run has none) | hekate (source TMR9, FIQ only, ended before launch); debug kernels (source TMR6, once `watchdogd` connects) |
| Reset path in the word | bit 15, `PMC2CAR` reset | the heartbeat's word has bit 15 too, plus bit 12 (interrupt) |
| Belongs to | the CCPLEX by convention: the Erista DTB's `watchdog@60005100` is WDT0 (disabled) | the BPMP: hekate's FIQ goes there, and the heartbeat comment says an FIQ or a core reset from it "would go to BPMP-Lite" |
| Collision | the panic path's `disarm_watchdog` writes unlock, disable and `TMR5_PTV` 0 to it, so a panic with a reboot delay would cancel ours; the shutdown watch arms it too | hekate arms it again in its next start; the kernel heartbeat refuses to arm over a timer block that is not idle |
| Source index | the panic path uses 5 with `TMR5_PTV` `0x60`; the DTB node names timer index 7 with a register at `0x60005088`, which the heartbeat's table gives to TMR0. The two do not agree and the TRM is not here | the heartbeat uses 6 with `TMR6_PTV` `0x68` |

Provisional preference, for the owner to decide after the read-only diagnostic shows both watchdogs idle: WDT0 on TMR5, the word the panic path already uses (no interrupt, `PMC2CAR` reset), because it is the watchdog Linux assigns to the CCPLEX, it avoids the BPMP's WDT4, and hekate does not touch it. Its collision with the panic path is confined to a kernel that has a reboot delay set, which the first-XNU kernel does not. WDT4 on TMR6 stays the alternative, with the heartbeat's word minus bit 12 (no handler exists in the loader or the early kernel).

## What the heartbeat code gives, and what it does not

Reusable as a sequence (GPL-2.0-only, the same repository):
- the unlock value `0xc45a`, and that a command write clears the unlock;
- the program order: unlock, `DISABLE` (command 2), timer `PTV`, config, `START` (command 1), because config ignores writes while the counter is enabled;
- the stop order: `START` to clear the interrupt status, then unlock and `DISABLE`, then `PTV` 0;
- the pre-arm classification (`kernel/switch/diag/heartbeat.h`): idle, dormant (enabled on a stopped timer, which is how hekate's `watchdog_end` leaves WDT4), or busy (timer running, an interrupt pending, or a shared watchdog status bit set). A read-only diagnostic can print this state without arming anything.
- the timing: a 250 ms timer tick, the period field counting ticks, a reset on the fourth expiry (the code's own comments, from Linux's `tegra_wdt.c`). With the panic path's use, `seconds << 4` gives about `seconds` in total; with the heartbeat's 48 ticks, 12 s per expiry and 48 s in total.

Not reusable: the kernel thread, the interrupt handler, `ml_io_map`, the panic and power-trail hooks. A loader step is a freestanding port of five register writes and a few reads.

Not shown: that any of it has been observed on an Erista, or on any console in a file read here. The heartbeat's commit message describes the intended timings; no result is recorded.

## TKE clock and reset

- `TIMERUS_CNTR_1US` at `0x60005010` is read by the first-XNU loader and by U-Boot (`boot/loader/stamps.h`: "free-running since SoC reset"), so the TKE block answers a read at EL1 on this console, and its counter runs.
- The CAR words measured at the loader (`CLK_OUT_ENB_L` `0x9802D131`, `RST_DEVICES_L` `0x049102CA`) were read before any diagnostic; whether the timer's clock enable and reset bits are among the set and clear bits is from Linux's clock table and not from a file here, so it is checked before an allowlist is written, not assumed.
- Whether the WDT registers (`0x100` and up) are readable and writable from EL1 non-secure on this console is not known. TMRUS being readable does not show it. A read that hangs the bus is the D0-S v1 precedent, so the read-only diagnostic reads TMRUS first and stops if anything else fails to return.

## PMC2CAR reset, the BPMP and the destination

- The word's bit 15 asks the PMC to reset the SoC through the CAR. That is all the repository knows: the comment in `heartbeat.h` ("TMR6, the interrupt and PMC2CAR's reset. No FIQ, no core reset") and `SwitchPanicSave.c` ("armed to reset through the PMC"). No source here states the conditions (a PMC enable, a lock, a secure setting) under which the reset acts.
- The reset cause it leaves is `PMC_RST_STATUS` (`0x7000e5b4`) 1, "the SoC watchdog" (`SwitchResetCause.h`, from L4T `pmc.c`); a software `MAIN_RST` leaves 3, THERMTRIP 2, power-on 0. That register is the evidence of what ended a run, and it is a PMC read this tree has only in the kernel, not in any loader.
- The BPMP: the design's own precedent avoids its FIQ and its core reset, so the path should not need BPMP software, but the sources do not show whether the BPMP firmware must be alive for the PMC to act. The first run keeps the BPMP running (hekate hands over to the BPMP firmware, which runs BL31 on a T210). Whether a hung or stopped BPMP changes the outcome is not tested and not testable offline.
- PSCI `SYSTEM_RESET`, the existing stop, is an `smc` to BL31 (`SwitchVmDone.c`), measured to return to hekate from the loader at EL1 in seven runs (`ERISTA_STOP_PATHS.md`). BL31 is not in this repository. A software main reset by the kernel's own `MAIN_RST` write is the panic path's, and hekate was seen to come back with RAM intact after it. Whether a watchdog reset, the PSCI reset and `MAIN_RST` land in the same place with DRAM kept is likely and unproven; `PMC_RST_STATUS` tells them apart afterwards.

## The state after a reset

Unknown, and it decides the loop question. Three outcomes are possible; each is observable with reads:
1. The TKE block returns to its reset state: the watchdog is idle. Nothing to clean up.
2. The block keeps the configuration but not the count: hekate finds an enabled watchdog on a stopped timer (the "dormant" state the heartbeat code already names), and must not arm over it blindly.
3. The block keeps both: the watchdog expires again in hekate, which is the loop.

No file here shows which one holds. The heartbeat's own comment says hekate's `watchdog_end` leaves WDT4 "enabled on a stopped timer", so at least a dormant state has been seen across hekate's use; that says nothing about a reset.

## What prevents a restart loop

Stacked, none proven:
- the watchdog lives in the SoC's own timer block, so a full SoC reset is expected to clear it, unlike a PMIC watchdog whose state may outlive the restart (reasoning; `ERISTA_T4_DIAG_PLAN.md` T4-A and T4-P are what read it back);
- the loader arms it only as the last step before the jump and never in hekate, in the diagnostics or in a release build;
- hekate's iOS entry is not an autoboot entry, so a reset lands in a menu;
- the period is long enough for a person to hold POWER, which ends any loop by removing power (a power-cycle resets the TKE block, which is also not read from a source here);
- the staged diagnostics run in an order in which a loop can first appear in a loader that does nothing else.

If hekate's `watchdog_fired()` is true at its next start, its bootloader skips its LP0 and Minerva step and the code read shows no `watchdog_end` on that path, so an armed leftover would also change how hekate starts. That is another reason the first state read comes before any arm.

## XNU initialisation and the TKE block

A search of this tree for the TKE block's address and the `TMRUS` register finds the loader's stamps, the OLED's suspend driver (after IOKit), the panic path and the heartbeat. The VM-stop run ends before IOKit and the suspend driver, and the heartbeat is armed only once `watchdogd` connects, so nothing in the run is known to write the block. The kernel's panic path touches WDT0 only when a reboot delay is set, and the first run sets none. This is a read of the sources in this repository, not of Apple's kernel code outside it, which the early path uses for other timers (the ARM generic timer).

## Open items that need the TRM or a measurement

Which source index each WDT takes (the DTB and the panic path differ); the exact expiry count and the effect of period 0; whether the WDT registers are accessible at non-secure EL1 here; the PMC2CAR conditions; the BPMP's part; the state after each kind of reset; whether the reset keeps DRAM through to hekate. None is settled by this document, and each has a place in the diagnostics.
