# Erista: what runs with the feature mask 0

Nothing here has run on an Erista (`boot 0`). Source reading only; file and line references are to the tree at this
commit. The Erista's mask stays 0.

## Gated now

Two features were added: `PMIC` and `MC_ERRORS`. The OLED has both, so it behaves as before. The Erista has neither.
They are not in a row's mask. The loader and the diagnostic images embed `switch_boards[]`, and the images' hashes are
pinned, so a wider mask would change them (checked: it did). `SwitchBoardKernel.h` names each row's extra features, and a
row without an entry does not compile.

| Feature | What it covers | Without it |
| --- | --- | --- |
| `PMIC` | `switch_pmic_start` (`SwitchPMIC.cpp`, called from `IOStartIOKit.cpp:92` in both XNU trees): I2C5 controller setup, the MAX77812 reads at `0x33` and `0x31`, the MAX77620 and RTC reads, the Wi-Fi clock, the PMC map and the halt and restart hook | The boot line reads the CAR only and says `VDD_CPU not read`. I2C5 is not touched. No `IORTC`, so `ArmDTPlatform` falls back to `IODTPlatformExpert` (calendar from the epoch). `PE_halt_restart` stays NULL, so `haltRestart` returns -1 |
| `MC_ERRORS` | `tegra_mc_errors_start`: maps the MC, writes MC_INTMASK, attaches INTID 109 | No MC write, no interrupt. `tegra_mc_error_latest` returns 0 (no lock), and `switch_mc_errors_resume` and `switch_mc_errors_detached` return without a source. MC faults go unreported |

`switch_board_start_source_test` pins both: nothing that uses I2C5 sits before the PMIC check in `switch_pmic_start`,
`readCpuRail` is read only behind it, and `SwitchRootBoot.c` has one driver without a feature (the watchdog).

Restart on the Erista needs a hook of its own. PSCI `SYSTEM_RESET` was measured to return to hekate, so a PSCI hook is
the candidate. It is not written: it needs an approval and a place (`ArmCpu` owns the conduit).

## Left running on every board

| Item | Hardware touched | Verdict |
| --- | --- | --- |
| `NativeSwitchWatchdog` (`switch_watchdog_publish`) | None in a release kernel. DEVELOPMENT and DEBUG arm WDT4 (TMR `0x60005000`) when watchdogd first connects (`kernel/switch/diag/heartbeat.c`) | Software supervision only: a one-shot timer that panics when watchdogd stops checking in. Not a hardware protection. It stays because launchd's `watchdogd` job crashes without an `IOWatchdog` |
| `switch_monotonic_clock_start` | Reads the CAR reset bit and the Tegra RTC counter in the PMC page. No write | Read only. Same block on both SoCs; not read on an Erista |
| `switch_panic_save_init` (`arm_init.c:551`) | Maps PMC and TMR, reads `PMC_RST_STATUS`. Writes PMC `SCRATCH37` to 0 only when it holds `TPAN`. Maps the panic region at `0xb0000000` | Board independent, same registers. The WDT0 reset is armed on panic and shutdown only (`SwitchPanicSave.c:510`) |
| `switch_board_read_hardware` (`SwitchBoardKernel.h`) | `switch_car_fuse_readable` sets the fuse clock enable and `MISC_CLK_ENB` visibility (CAR writes), then reads HIDREV and ODM4 | Needed to name the board before any gate can run. The loader read the same registers on the Erista (D0) |
| SD block driver | SDMMC1 | The UHS re-initialisation, with its I2C5 and PLLC4 access, is skipped below HIDREV major 2 (`NativeSwitchBlock.cpp:3715`). See `ERISTA_ADT.md` |
| ArmGIC, ArmCpu, ArmDTPlatform | GIC-400 and the PSCI conduit, from the ADT | Required. GIC words were read by D0 |
| NVRAM | None (RAM until Data mounts) | |
| `native_legacy_display_probe` | Unknown: no definition in this tree | Default off per its comment. Unaudited |
| `sheng_pmgr_run` | The emulator kit only (`kits/qemu`) | Not in a hardware kernel |

### Open

- The watchdog's first open waits up to 180 s for `switch_sensors_wait_ready` (`NativeSwitchWatchdog.cpp`). A board
  without the SENSORS feature never gets ready, so launchd's `watchdogd` would wait the full 180 s and then run with
  monitoring off. Skipping the wait on such a board is a behaviour change. Not made.
- A DEVELOPMENT or DEBUG kernel arms WDT4 on any board when watchdogd connects. Release kernels do not.
- Whether the Erista's Tegra RTC and `PMC` scratch registers behave as the OLED's is not measured.

## random-seed

The loader mixes SE2's DRBG output into `/chosen/random-seed` (`seed.c`). The Erista has no SE2, so the pinned seed is
used unchanged and every boot starts XNU's early generator from the same state (`pe_gen.c:178`, `arm_init.c:682` stack
guard). Candidates, in the order to try them:

| Source | Per boot | Needs | Open |
| --- | --- | --- | --- |
| SE1 RNG (`0x70012000`) from the loader, as `seed.c` does for SE2 | Yes | SE clock and entropy enable, one DRBG operation: register writes | Whether EL2 non-secure may run an RNG operation on the Erista is not known. A read of the SE security registers has to come first. SE1's key slots belong to the AppleKeyStore stand-in, so the RNG configuration must not disturb them |
| A seed file on the SD card, refreshed from the kernel after each boot | Yes, from the second boot | A writable place the loader can read; no MMIO | The first boot has nothing. Anyone who reads the card has the seed. No forward secrecy |
| Console fuse ID (ECID, read only) and counter jitter at several loader checkpoints | No: unique per console, near constant per boot | Fuse reads | Low entropy. Only a stopgap |
| BL31 or U-Boot supply | Unknown | | Not checked |

The first row is the real fix. The loader has 31 bytes of room below the stamps slot (see `NOTES.md`), so none of this
goes into `loader.c`. It belongs in a kernel driver that reseeds the kernel PRNG after boot, as `tegra_se_start` does
for SE2, with the file as the fallback until then. Nothing is written yet.
