# D0-S: run sheet

D0-S ran once and hung after the framebuffer line (`boot/diag/erista-d0s-run1.txt`): FAIL / PARTIAL. Do not run it again. Its replacement is D0-S v2 (`ERISTA_D0S_V2_DESIGN.md`), which is not approved to run. One run, no retry, `boot` stays 0.

D0-S is D0-R's loader behind a U-Boot script that adds one read-only block before the handoff. It prints the control FDT's
framebuffer properties and reads four words: CAR `RST_DEVICES_L` (`0x60006004`) and `CLK_OUT_ENB_L` (`0x60006010`), then SDMMC1
`0x700b0024` and `0x700b002c` only if the CAR words say SDMMC1 has its clock on and is out of reset. It writes no register and
nothing to the card. The RAM it writes is what D0 already writes: the DTB copy at `0x8D000000`, one reserve-map entry and the
loader at `0xA0000000`. `diag_state_audit_test` pins this list on the rendered script.

## 1. Build and hash (Mac)

```
bazel test //tools/package/test:diag_state_audit_test //tools/package/test:diag_state_test --config=ios17
bazel build //boot:diag_state //boot:diag_d1min //boot:diag_loader_r --config=ios17
PKG="$PWD/bazel-bin/boot/diag_state/package"
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256)
shasum -a 256 bazel-bin/boot/diag_state/loader.uboot.bin bazel-bin/boot/diag_d1min/loader.uboot.bin \
  bazel-bin/boot/diag_loader_r/loader.uboot.bin
```

The package has four files: `bootloader/ini/ios-diag-state.ini` and `switchroot/ios-diag-state/{bl31.bin,bl33.bin,script.txt}`.
`bl31.bin` equals the other diagnostics' (`dea0ecd4...34e2b4`).

The three `loader.uboot.bin` files are equal. The Mac build of 2026-11-26 gave
`cdc52c651a0ae8a8e21d1932f63813702131dc0e91db7a1d2883890ebd59cd2a` for all three. That is not the
`c101d9c3...` of `tools/package/diag_images.py`: that one is a flat image built on Linux with clang 18.1.3, and the Mac file is a
U-Boot image (64-byte header plus the flat image) built with Xcode's clang. Only same-toolchain builds compare.

To compare with the D0-R of the hardware run on the same toolchain:

```
git worktree add ../before 5f474ed0d
(cd ../before && bazel build //boot:diag_loader_r --config=ios17)
shasum -a 256 ../before/bazel-bin/boot/diag_loader_r/loader.uboot.bin bazel-bin/boot/diag_loader_r/loader.uboot.bin
```

Equal means the loader is the one that ran. A different value needs a look before the card is written.

Record the four package hashes in the review before copying. They are not written here because no Mac run reported them yet.

## 2. Copy and check (Mac)

`cp -n` refuses to replace a file. If it prints nothing and the check below fails, a file of that name already existed.

```
export COPYFILE_DISABLE=1
SD="/Volumes/CARD"
ls "$SD/switchroot/ios-diag-state" "$SD/bootloader/ini/ios-diag-state.ini" 2>&1
mkdir -p "$SD/switchroot/ios-diag-state"
cp -n "$PKG"/switchroot/ios-diag-state/bl31.bin "$PKG"/switchroot/ios-diag-state/bl33.bin \
  "$PKG"/switchroot/ios-diag-state/script.txt "$SD/switchroot/ios-diag-state/"
cp -n "$PKG"/bootloader/ini/ios-diag-state.ini "$SD/bootloader/ini/"
sync
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256) > /tmp/d0s.sums
(cd "$SD" && sed 's|  \./|  |' /tmp/d0s.sums | shasum -a 256 -c -)
find "$SD" -name '._*' -path '*ios-diag-state*'
```

The first `ls` should say `No such file`. All four lines of the check print `OK` and `find` prints nothing. Eject with
`diskutil eject "$SD"`, insert again and run the check a second time. Any `FAILED`, missing file or `._` file: do not run.

## 3. Before starting

| Check | |
| --- | --- |
| Experiment card only, backed up | the `iOS`, `iOS Diag`, `iOS Diag Memory` and older diagnostic entries stay as they are |
| hekate 6.5.5, Auto Boot off | nothing in `bootloader/ini` starts on its own |
| Temperature | cold start: off for 10 s, then hekate. Read Nyx's temperature. Start only below 40 C |
| Battery, placement | charged, charger out, hard surface, vents clear, no case, Joy-Con off |
| Camera | recording from before `More configs` until hekate is back |
| Someone at the console | within reach of POWER |

The 40, 50 and 55 C figures and every deadline below are provisional. No limit for this console is known.

## 4. The run

Launch `iOS Diag State` from Nyx "More configs". Film the screen from the first U-Boot line.

Expected U-Boot lines, in order:

```
D0-S STATE DIAGNOSTIC. READ ONLY. NO KERNEL. NO BOOT
BOARD OK
FRAMEBUFFER OK
DTB COPIED AND FRAMEBUFFER RESERVED
LOADER WRITTEN TO 0xA0000000
D0-S FRAMEBUFFER PROPERTIES: ADDRESS f5a00000, SIZE PROPERTY LENGTH 4, 2d0 X 500, a8b8g8r8, ROT 3
D0-S CAR RST_DEVICES_L <hex>, CLK_OUT_ENB_L <hex>
D0-S SDMMC1 PRESENT STATE <hex>, WORD AT 0x2C <hex> (BYTE 3 IS SOFTWARE RESET)
HANDOFF TO THE LOADER. THE SCREEN CLEARS, THEN THE REPORT
```

The hex digits of the framebuffer line are U-Boot's formatting and may differ in case or leading zeros.

Then the screen clears and the loader prints D0-R's report: `HIDREV 00022117`, `ODM4 00000204`, `BOARD ERISTA T210B01=0 SKU=0 BOOT=0`,
`EL1 STATE PASS`, `ISAR0 ... CRC32 1 OK`, `MC REGISTERS READ 25 DIFFERENT FROM THE OLED 0`, `FDT ... RESERVATIONS 3`,
`FDT WALK CODE 0 ALL RANGES FREE`, banks, `MAP 0` to `MAP 2`, `NODE 0`, `EXPORTER ... CLEAR`, `SD REFERENCE ... CLEAR`, six `GIC`
words, `BOOT STAYS 0`, `RESET IN 15 S`. The values equal `boot/diag/erista-d0r-run.log`.

What the two CAR words should show: bit 14 (`0x4000`) is SDMMC1 in both. If hekate left the controller on, reset reads 0 and clock
reads `4000` and the SDMMC1 line appears. If it did not, the line `D0-S SDMMC1 CLOCK OFF OR IN RESET: NOT READ` appears instead.
Both are valid results. Neither is a failure.

## 5. End conditions and recovery

The run ends by itself: 5 s after the SDMMC1 block, the loader draws its report and counts 15 s, then PSCI `SYSTEM_RESET`
returns to hekate. Total about 40 s from the first line. Return to hekate is expected, as in the D0 and D0-R runs.

| What you see | Do |
| --- | --- |
| Report, then hekate's menu after about 15 s | Done. |
| Screen stops at `HANDOFF TO THE LOADER` and never clears | Treat as a hang. Hold POWER until the console is off. Report the last line. |
| Nothing after the entry is selected for 20 s | Hold POWER. Report. |
| `PSCI SYSTEM_RESET RETURNED`, `NO RESET`, `NO EL CHANGE`, `EXIT CALLED OUTSIDE EL1` | Hold POWER. |
| The console feels hot or the screen shows garbage | Hold POWER at once. |
| Any deadline in `boot/diag/loader/RUN_CHECKLIST.md` passes | Hold POWER. Do not retry on the spot. |

Recovery is the held power button, then hekate as usual. There is no other stop: a hang has no hardware reset
(`ERISTA_STOP_PATHS.md`). Do not press anything else during the wait.

### Failure codes

U-Boot prints `REFUSED: <status>`, waits 15 s and runs `reset`. Nothing is booted.

| Status | Meaning | What it left in RAM |
| --- | --- | --- |
| `WRONG_BOARD` | the board gate failed | variables only |
| `CONSOLE_FAILED` | `vidconsole` not set | variables only |
| `CONTROL_FDT_FAILED`, `FRAMEBUFFER_PROPERTY_FAILED`, `FRAMEBUFFER_MISMATCH` | the control FDT lacks the framebuffer node or it is not `F5A00000`, 4, `2D0`, `500`, `a8b8g8r8`, rot 3 | variables only |
| `DTB_COPY_FAILED` | the copy to `0x8D000000` failed | possibly a partial copy |
| `DTB_HEADER_FAILED`, `DTB_PADDING_FAILED`, `FRAMEBUFFER_RESERVE_FAILED` | the copy's header or padding, or the reserve entry | the DTB copy, reserve map possibly half edited |
| `BOOT_ENVIRONMENT_FAILED`, `BOOT_HEADER_FAILED` | `bootm start` could not take the loader | the DTB copy |
| `IMAGE_LOAD_FAILED` | `bootm loados` failed | the DTB copy, part of the loader |
| `BOOT_PREP_FAILED`, `UNEXPECTED_BOOTM_RETURN` | `bootm prep` failed, or `bootm go` returned | the DTB copy and the loader |

The D0-S lines `CAR READ FAILED`, `CAR BIT TEST FAILED` and `SDMMC1 CLOCK OFF OR IN RESET` are not refusals. They only skip the
SDMMC1 reads and the run goes on. `CAR BIT TEST FAILED` would mean `setexpr` with `&` did not work under U-Boot's shell; that
is a finding to report. A status not in the table: assume everything above was written. A reset or power-off discards the RAM.

Loader-side stops (`D0 ENTRY CHECK FAILED CODE`, `EL1 STATE FAILED. NO DEVICE READS`, `FDT CHECK CODE`, `RESERVATION OVERLAPS`) are D0-R's
and are described in `boot/diag/loader/RUN_CHECKLIST.md`. The countdown still applies.

## 6. Temperature

The exposure is U-Boot's few seconds, the 5 s and the loader's 15 s with the fan off, about D0-R's. Nothing in D0-S manages the
fan or reads a sensor. After the run: photograph first, then Nyx's temperature. Do not start another run until the console is
below 40 C again. Above 50 C at the end: leave it off. Above 55 C: stop the session and report. All three are provisional.

## 7. Effect on existing files

| Where | Change |
| --- | --- |
| SD card | two additions: `bootloader/ini/ios-diag-state.ini` and `switchroot/ios-diag-state/`. The script writes nothing to the card |
| Other entries and `iOS` | untouched. `boot` 0 means the iOS entry cannot start either way |
| RAM | the D0 writes listed above, gone at reset |
| U-Boot variables | set, never `saveenv`, gone at reset |
| Console flash, eMMC | not touched |
| Registers | four reads, no write |

After the run: remove `switchroot/ios-diag-state/` and the `.ini`, then re-run the `find` for `._` files. D1-min only after this
run's photograph is reviewed.

## 8. Rejection tests (first-XNU package)

These check that the first-XNU package cannot be built without the trace and cannot be built for release. They do not run a
kernel. A failure is the pass condition for the middle two.

```
bazel build --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true //boot:erista_first_xnu            # builds
bazel build --config=ios17 --//install:prebuilt=local //boot:erista_first_xnu                                       # fails in first_xnu_check, no early-trace.ok
bazel build --config=ios17 --config=release --//install:prebuilt=local //boot:erista_first_xnu                      # fails at analysis, needs the debug profile
bazel build --config=ios17 --//kernel:early_trace=true //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel <that kernel>                                                      # exit 0
```

Build `//kernel:prelinked` again without `--//kernel:early_trace=true` and run the same check: exit 1. The label and flag names are
from `ERISTA_FIRST_XNU_BOOT.md`. None of this ran on the Mac for the seeded target yet.
