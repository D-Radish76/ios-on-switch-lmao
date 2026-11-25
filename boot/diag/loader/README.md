# D0 loader diagnostic

The first stage of running the normal loader's path on an Erista before any kernel exists. It is a separate artifact:
its own loader, its own `bl33.bin` and its own hekate entry, `iOS Diag Loader`. The normal `iOS` entry, its loader and
the `boot` flag in `kits/SwitchBoard/SwitchBoard.h` are not touched, and the Erista's row stays `boot 0`.

It carries no kernel window, no exporter, no `bootargs.txt` and no `loader_enter_kernel`. `bazel build //boot:diag_loader`
refuses to link a binary that does (see Inspection).

## Build and run

```sh
bazel build //boot:diag_loader --config=ios17
```

Copy `bazel-bin/boot/diag_loader/package/{bootloader,switchroot}` onto the SD card's FAT. Nothing existing is
overwritten. In hekate choose `More configs`, then `iOS Diag Loader`. Photograph the screen; after 15 s the console
resets. If the screen stays unchanged for 20 s, hold the power button.

## What it does

U-Boot runs the normal script's checks up to `bootm prep`, then `bootm go` enters the loader at EL2. The loader:

1. Validates the FDT pointer (the page the script copied the DTB to, inside DRAM, clear of the loader's image), the FDT
   header and the reservation block, and then the framebuffer: whole inside DRAM, clear of the image and covered by one
   FDT reservation. Without all of that it draws nothing and reads nothing else.
2. Checks the EL2 entry state, makes the normal loader's EL2 to EL1 change, and checks the EL1 state.
3. Reads HIDREV and FUSE_RESERVED_ODM4 and names the board row. An unknown chip skips the register reads below.
4. Reads the 25 memory controller registers and the six GIC words `native_gate` reads, and prints each against the OLED
   value. All reads go through `switch_diag_read32`, one load. The memory controller is never written.
5. Reads ISAR0, the entry stack pointer and runs the normal loader's FDT memory walk over the loader image, the SD
   reference page, the staged-window capacity, the kernel destination and the boot-args page.
6. Waits 15 s and calls PSCI `SYSTEM_RESET`.

Skips: a failed FDT or framebuffer check draws nothing and reads nothing; a failed EL1 state or an unknown board skips the
MC and GIC reads; a failed FDT check skips the memory walk.

## The exit

The wait is the CPU counter (`CNTPCT_EL0`), not a peripheral timer. The core sleeps in `wfe`; the generic timer's event
stream wakes it every 3.4 ms to compare against the deadline.

Before waiting, `CNTFRQ_EL0` must be exactly 19200000 and the counter must advance within 1000 reads. If not, the screen
says so and the core parks in `wfe` with all exceptions masked. Nothing resets, because the wait cannot be bounded. The
same park follows an EL2 entry check that fails, and a reset call that returns.

Unverified on hardware: PSCI `SYSTEM_RESET` from the loader at EL1 (U-Boot made the call from EL2 and it worked), and the
event stream waking `wfe`. If either fails the screen stays; hold the power button.

## CPU state it changes

Everything the diagnostic writes outside RAM is a system register. `tools/package/diag_loader_inspect.py` holds the
list and the link fails when the binary writes anything else, or a listed register a different number of times.

| Register | Write | Why |
| --- | --- | --- |
| `DAIFSet` | mask D, A, I, F | no interrupt or abort can interrupt the sequence |
| `SPSel` | SP_ELx | the private stack, not U-Boot's |
| `VBAR_EL2`, `VBAR_EL1` | the loader's vectors | a fault is reported on the screen |
| `CNTHCTL_EL2` | 3 | EL1 may read the counter |
| `CNTVOFF_EL2` | 0 | no virtual counter offset |
| `VPIDR_EL2`, `VMPIDR_EL2` | real MIDR, MPIDR | EL1 reads the real IDs |
| `CPTR_EL2`, `HSTR_EL2` | no traps | EL1 is not trapped to EL2 |
| `HCR_EL2` | `0x80000000` | EL1 is AArch64; no stage 2, traps or interrupt routing |
| `CPACR_EL1` | 0 | FP/SIMD stays off; the code is integer only |
| `SCTLR_EL1` | `0x30d00800` | MMU, caches and alignment checks off |
| `TTBR0_EL1`, `TTBR1_EL1`, `TCR_EL1`, `MAIR_EL1`, `CONTEXTIDR_EL1` | 0 | reset values; no translation |
| `SP_EL1` | the private stack | the stack at EL1 |
| `SPSR_EL2` | `0x3c5` | EL1h, all masked, for the `eret` |
| `ELR_EL2` | the EL1 half of the entry | where the `eret` lands |
| `CNTKCTL_EL1` | 0, then `0xf4` | 0 at EL1 entry; then the event stream |

The EL transition is one `eret` from EL2 to EL1h, with `tlbi vmalle1` before it. The sequence is
`boot/handoff/entry.S`, the one the normal loader runs on the OLED; this build includes that file rather than copying it.
After the `eret` there is no way back to EL2. `smc #0` is the only instruction that leaves EL1 for firmware.

## Inspection

`link` in `tools/package/diag_loader.py` fails unless:

- the link map has none of the normal loader's kernel path (`loader_enter_kernel`, the block table, copy, EMC, seed,
  boot-args, SD reference, LZ4, exporter) and has the four functions the report needs;
- the disassembly has exactly the system register writes above, one `eret`, one `smc`, one `tlbi`, two `wfe`, and none of
  `hvc`, `svc`, `ic`, `dc`, `at`, `sys`, `wfi`, `sev`;
- no instruction builds an address in `0x5000xxxx`..`0x7fffxxxx` other than the two identity registers' `0x7000`;
- `switch_diag_read32` is one load and a return.

A static scan cannot prove no store reaches a device. The guarantee is the last two rules plus
`diag_loader_test.py`, which fails if a source stores through a volatile cast or forms a device address anywhere but
`switch_diag_read32`. `bazel-bin/boot/diag_loader/{loader.map,loader.dis,inspection.txt}` hold what was inspected.

## Why a second artifact

It reuses `boot/loader/memory.c` and `boot/handoff/{entry.S,display.h,record.h}` unchanged, so what it reports is what
the normal loader would see. Everything else is its own, so it cannot start a kernel by a mistake in the normal loader's
conditionals. D1 (exporter and SD reference, read only) and D2 (staged window header and CRC, no copy) come after D0 is
reviewed on hardware.
