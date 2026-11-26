# Erista: XNU handoff

iPadOS 17 (xnu-10063.141.1) first. iOS 27 is not covered. The Erista row stays `boot 0` with feature mask 0. Nothing here
boots XNU on hardware, writes an MC register or changes a voltage, EMC or DFLL setting. T1 v3 further boots, T2 to T4, D1
and T1-H stay on hold.

## What is in place

| Area | State | Where |
| --- | --- | --- |
| Board gate | `native_gate` names no board; carveouts per row; the Erista row refused at `loader.c:292` (0x306) and by U-Boot's script (WRONG_BOARD) | `boot/loader/carveout.h`, `carveout_gate_test` |
| Erista ADT | One CPU, no PSCI node, audited nodes only | `ERISTA_ADT.md`, `native_erista_adt.py`, `adt_check.py` |
| Package | `//boot:erista_package`, manual, own directory | `boot/package.bzl`, `erista_package_test` |
| Feature gates | PMIC and MC_ERRORS per row, in `SwitchBoardKernel.h`; the Erista has neither | `switch_board_start_source_test` |
| Sensor wait | Skipped without SENSORS (was 180 s) | `NativeSwitchWatchdog.cpp` |
| What still runs on every board | `ERISTA_BOOT_AUDIT.md` | |

Mac-verified at 10f31e883: `//boot:package`, `//boot:erista_package`, `//boot:erista_loader_all_rows`, the host tests
(`ERISTA_ADT.md`).

## N6-1: checkpoints

No new hardware initialisation is needed for any of these; each uses a mechanism that exists.

| Checkpoint | Observed by | Visible without a debug console |
| --- | --- | --- |
| Before `loader_enter_kernel` | Loader words (`probe_record`) and the stamps at `0xa001ef00` (`stamps.h`) | No. Read from RAM after a reset |
| XNU entry, MMU, exception vectors | `SWITCH_EARLY_MARK` 1 to 6 in `start.s`: stage into `switch_early_record[1]`, a 16x16 block at y 280, x `16 + 40 * (stage - 1)` | Failure colours only. A green mark is not drawn |
| `arm_init` | `switch_diag_early_stage` 7 | On an early exception or panic screen (LAST STAGE) |
| `PE_init_platform` | stage 9 (platform expert ready) | Same |
| Early memory | stages 16 to 21 | Same |
| GIC init | `switch_diag_stage` "platform interrupts initialized" (`arm_init.c:621`) | DEVELOPMENT console only |
| IOKit init | `bsd_init entered` (`arm_init.c:486`), `switch_root_record_stage` | DEVELOPMENT console only |
| First storage access | SwitchSD logs, `switch_root_event` 6 and 7 | DEVELOPMENT console only |

The gap is a hang before the console exists with nothing wrong enough to raise an exception: the screen stays as the
loader left it and the stages exist only in RAM. Two designs close it, neither implemented (`start.s` is not changed
without approval):

1. A DEVELOPMENT-only variant of `SWITCH_EARLY_MARK` that also draws the green block, and a block per stage 7 to 21
   at y 300. Framebuffer stores only, the same ones the failure marks already make.
2. Reading `switch_early_record` from RAM after the reset (hekate can dump DRAM; unverified for this layout).

Release builds keep neither (`DEVELOPMENT || DEBUG` gate). Which stages a hang reaches therefore needs a DEVELOPMENT
kernel.

## N6-2: stop paths

| Path | Today | Distinguished by |
| --- | --- | --- |
| Panic | `SwitchPanicSave.c`: record in RAM at `0xb0000000`, `TPAN` in PMC `SCRATCH37`, WDT0 (TMR5 via PMC2CAR) armed to reset | the record and `PMC_RST_STATUS` on the next boot |
| Hang | No software supervision in a release kernel: `NativeSwitchWatchdog` drives no hardware. DEVELOPMENT or DEBUG arms WDT4 | not distinguishable after a power cycle; with WDT4 the reset cause says watchdog |
| Normal stop | `reboot(2)` to `halt_all_cpus` to `PEHaltRestart`. With no PMIC the hook is NULL (`haltRestart` returns -1) | `get_system_inshutdown`; `SHUTDOWN_REBOOT_S` arms the WDT0 reset after a shutdown has begun |

Software supervision is the `watchdogd` heartbeat: it panics when `watchdogd` stops checking in. It is not autonomous
hardware protection. The only autonomous resets are WDT0 on panic and shutdown, and WDT4 in DEVELOPMENT builds. A release
kernel on the Erista has no reset of its own for a hang, so a real-hardware boot would need the power button held.

Restart path, design only: PSCI `SYSTEM_RESET` (`0x84000009`) returned to hekate on the Erista when measured. A hook would
live in `ArmCpu`, which owns the conduit (`psciConduit`, `psciCall`, from `/psci`). The Erista ADT has no `/psci` node
(`ERISTA_ADT.md`), so the conduit would have to come from the ADT first. The PMC `MAIN_RST` fallback is the one
`SwitchPanicSave.c` already uses. Unverified: whether `SYSTEM_RESET` is reached from EL1 after XNU has taken over.

## N6-3: diagnostics

One read-only image can print the framebuffer values (`loader.c:485`) and read the SD engine words. Skippable because
measured already: EL1 state, ISAR0, entry SP, GIC words, the 25 MC registers, DRAM banks. The framebuffer check passed
by control flow in D0 and D0-R (inferred, not printed). D1 is cut into D1-min, D1-b, D1-c and D1 (`ERISTA_NEXT_DIAGNOSTICS.md`).

Smallest plan: one run of the read-only image, one D1-min, and a D1 only after those two are reviewed. Whether the script
expresses D1-min and D1-b by dropping steps from the D1 template is unchecked. None is approved.

## N6-4: before a real-hardware boot

Random seed: the pinned seed is reused on every boot (no SE2). Options are in `ERISTA_BOOT_AUDIT.md`. The loader has no
room, so it is a kernel driver. Unverified: SE1 RNG access from EL2 on the Erista. CRC32 is not tamper protection.

Open items by what settles them:

| Item | Settled by | Hardware |
| --- | --- | --- |
| T2: TMP451 register `0F` write form, offset register addresses | data sheet | no |
| T2: BUSY during a one-shot, ACK at the changed divisor | one T2 run | yes (hold) |
| T3, T4 safety mechanisms (stop conditions, temperature limits) | a design and its approval | not yet designed here |
| Erista temperature limits | not known. The criteria in the T1 documents are provisional, not official | |
| RTC and PMC scratch behaviour | not measured | yes |

Conditions that must all hold before anyone sets `boot 1`:

- T2 to T4 designed, approved and run, or explicitly waived by the user.
- A seed source decided and implemented.
- A reset path for a hang (WDT or a held button procedure) decided.
- The Mac list below green on the same commit, with the Erista ADT's one-core check.
- A release prebuilt decision: the release's loader and window are the OLED's.

Package contents: `//boot:erista_package` carries the Erista ADT and DTB, one core and the loader built from this tree.
Refusal at `boot 0`: U-Boot's script (`boot_script_board_test`) and the loader (`carveout_gate_test`). Under the release
prebuilt, the bl33 stage refuses the target because `constants.json` differs; build with `--//install:prebuilt=local`.
That refusal has not been run on the Mac.

## N6-5: regression checks

Local, this commit: the four diagnostic loader images I can build here (D0, D0-R, D1, T1) hash as before; the three
others are for the Mac. `loader.c` with the loader flags and a stub `layout.h`: text 5112, rodata 640 + 202, data 280, bss
0; with every row's MC values +96 bytes. The linked margin is for the Mac. Bazel did not run here: the Python and g++
checks use stand-ins and prove attributes and logic, not Starlark validity.

```
bazel test --config=ios17 //tools/package/test:erista_package_test //tools/package/test:adt_check_test \
  //kits/SwitchBoard/test:all //drivers/SwitchPower/test:switch_board_start_source_test \
  //boot/loader/test:carveout_gate_test
bazel build --config=ios17 //boot:package //boot:erista_loader_all_rows
bazel build --config=ios17 --//install:prebuilt=local //boot:erista_package
shasum -a 256 bazel-bin/boot/full-ram.adt        # equals the value from 85757fc66
python3 tools/package/adt_check.py --cores 1 <the Erista ADT>
```

The seven diagnostic images are built as in `ERISTA_T1V3_RUN.md` section 1 and compared with
`boot/diag/erista-diag-image-hashes.txt`.
