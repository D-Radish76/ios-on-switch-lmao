# Erista: restart, stop and reset paths

Design and host checks (N7-C). No PMC, PMIC or watchdog operation is added, and `boot` stays 0.

## How each end of a boot looks on the Erista today

Single core, no PMIC (feature mask 0), release kernel, no sensor driver.

| End | What happens | Told apart afterwards by |
| --- | --- | --- |
| Panic | `SwitchPanicSave.c` keeps the record in RAM at `0xb0000000`, and 60 s after a watchdog panic (or `DARK_REBOOT_S`, `SHUTDOWN_REBOOT_S`) arms WDT0 and writes PMC `MAIN_RST`. With no timer the panic screen stays up | the saved record; `PMC_RST_STATUS` 3 (software) |
| Hang | Nothing resets it. `NativeSwitchWatchdog` is software supervision: `watchdogd` checks in, a missed check panics. A hang of the kernel itself has no supervisor in a release kernel. Debug kernels add WDT4 (the freeze heartbeat), armed once `watchdogd` connects | after a power cycle only the power-on cause; with WDT4, `PMC_RST_STATUS` 1 and a filed report |
| Normal stop | `halt_all_cpus` calls `PEHaltRestart`, which returns -1 because `PE_halt_restart` is NULL without the PMIC, and the next statement is `while (1)` (`machine_routines.c`). The console stays up until POWER is held | not recorded: the next boot is a power-on |
| Thermal | `SwitchThermal` is not started without SENSORS. The only protection is hardware THERMTRIP, if something programmed it. Nothing here does | `PMC_RST_STATUS` 2 |

The hang and the normal stop are the gaps. Software supervision (`watchdogd`, the 60 s panic reset) needs a running kernel; only a
timer that nothing but a running kernel can restart resets a dead one, and release builds arm none.

## PSCI SYSTEM_RESET

Measured on the Erista: PSCI `SYSTEM_RESET` (`0x84000009`) returns to hekate. The kernel cannot reach it today:

- `ArmCpu::psciConduit` reads `/psci` from the ADT, and on a GICv2 board it also requires `gNativeSmp`. The Erista tree is one core with
  no `/psci` node (`native_erista_adt.py` refuses PSCI), so no conduit exists.
- `PE_halt_restart` is installed by `SwitchPMIC` only.
- The panic reset does not use PSCI: it writes PMC `MAIN_RST` itself.

Design, in the order a later approved change would take:

1. The Erista ADT gains `/psci` (`method` `smc`, `compatible` from the firmware's own `PSCI_VERSION`, which has not been read on this
   board) while staying single core. `adt_check.py` and `native_erista_adt_test` change with it.
2. `ArmCpu` exports `SYSTEM_RESET` with a conduit that does not depend on `gNativeSmp`; `CPU_ON` and the sleep calls keep that condition.
3. A `PE_halt_restart` hook for boards without the PMIC, behind its own kernel feature (`SwitchBoardKernel.h`, per row, so the Erista
   stays 0 until approved): `kPERestartCPU` calls `SYSTEM_RESET`; panic callouts return -1 as `SwitchPMIC` does.
4. `kPEHaltCPU` has no power-off to offer: `SYSTEM_OFF` panics in the switchroot ATF ("operation not handled"). Either it also resets
   (to hekate, where the console can be switched off by hand) or it keeps spinning. A decision for the owner.

Not verified: that `SYSTEM_RESET` works from EL1 after XNU owns the core, and what the board does with DRAM contents across it (hekate
saw RAM intact after the panic path's `MAIN_RST`, not after PSCI).

## What an autonomous reset needs

Nothing may be armed until each is settled:

1. The register sequence, from the TRM or a measured read-only dump, with its unlock and the reset destination (WDT0 to PMC is the one
   path the panic code already uses; WDT4 is the BPMP's and hekate's).
2. A kicker that only a healthy kernel runs, and nothing that stops the timer on its own.
3. Arming before userspace, and disarming on a normal stop so a restart is not a reset loop.
4. A first run that proves the reset lands in hekate.
5. A period longer than the slowest legitimate stall, from a measurement; the 30 s and 50 s figures are provisional.

## Host checks

`diagnostics/switch/test/stop_path_source_test.py` pins the facts above from the sources, so a change to any of them shows up as a failing test
rather than a stale document.
