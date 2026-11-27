# T3-R: build, check and (later) run sheet

`//boot:diag_t3r` is built and tested offline. It is **not approved to run**: no hardware run, `boot` stays 0, the feature mask stays 0, T3 and T4 are not waived, the first XNU run is on HOLD. What it reads and why: `ERISTA_T3R_DESIGN.md`.

## What the image does

D0's loader, then 22 allowlisted reads, each a 32 bit word, each drawn and settled for 100 ms before it is made, with a RESULT line after it. The 21 addresses:

| Block | Addresses |
| --- | --- |
| TKE | `0x60005010` (`TMRUS`, read first and last) |
| CAR | `0x60006004`, `0x60006010` |
| PWM | `0x7000A010` |
| Pinmux | `0x7000320C`, `0x700031EC`, `0x7000304C`, `0x700031A8` |
| GPIO | `0x6000D000`, `0x6000D010`, `0x6000D020`, `0x6000D700`, `0x6000D710`, `0x6000D720`, `0x6000D408`, `0x6000D418`, `0x6000D438`, `0x6000D504`, `0x6000D514` |
| PMC | `0x7000E444`, `0x7000E4E4` (last) |

The PWM read is made only when the PWM clock is on and its reset released. There is no write to a device register anywhere in the image, and the fan and the 5 V rail are untouched. It ends the way every diagnostic does: the shared exit, a 15 s dwell, then PSCI `SYSTEM_RESET`.

## What a result does not show

- **A read that hangs.** The 20 s read budget is checked between reads only, and no software timeout leaves a hung read. A bus hang leaves the screen on the BEGIN line of that register; the person holds POWER. POWER hold is not asserted as a certain recovery.
- **GPIO and PMC reads at non-secure EL1 are unconfirmed.** No earlier run read these blocks. Pinmux pads have a precedent (T1's I2C1 pads), the others do not.
- **The state is the one after hekate and U-Boot.** The predictions assume Nyx ran `hw_deinit`. A difference from them is information, not a fault.
- **A fan turning.** One `IN` sample of S7 cannot show it. A DUTY line reads hekate's formula only.
- **A time limit is not a safety guarantee.** The 20 s budget, the 30 s no-new-line rule and the provisional 60 s from selection to hekate's menu are not measured on this image.

## Person's rules for the run

Selection of `iOS Diag T3R` to hekate's menu should take about 26 s by the arithmetic in the design (an assumption for the return). No new line for 30 s: hold POWER. No hekate menu after 60 s from selection: hold POWER. Video with a clock in frame, so the first line, the line interval and the return time can be read off it.

## Offline checks

From the repository root:

```
python3 tools/package/diag_images.py                       # eleven images; the ten earlier hashes must not change
python3 tools/lint/runfiles_check.py diag_t3r_test
PYTHONPATH=tools/package python3 -m unittest tools/package/test/diag_t3r_test.py
```

The Mac, with Bazel:

```
bazel build --config=ios17 //boot:diag_t3r
shasum -a 256 bazel-bin/boot/diag_t3r/loader.uboot.bin
cat bazel-bin/boot/diag_t3r/inspection.txt
bazel test --config=ios17 //boot/diag/loader/test:t3r_test //tools/package/test:diag_t3r_test \
  //tools/package/test:diag_t4r_test //tools/package/test:diag_loader_test //tools/package/test:diag_mmio_audit_test \
  //tools/package/test:diag_remaining_docs_test //tools/package/test:diag_t3r_design_test
python3 tools/package/diag_mmio_audit.py --disassembly bazel-bin/boot/diag_t3r/loader.dis --uboot-image bazel-bin/boot/diag_t3r/loader.uboot.bin --t3r
```

The audit must end with `AUDIT PASSED (T3-R)`. The Linux flat hash is `075d6816...` after the 2026-11-27 fixes (`boot/diag/erista-diag-image-hashes.txt`; `fc904cfc...` before them); the Mac's flat hash is in the Mac record below, and its `loader.uboot.bin` is a different file. `boot/diag/erista-t3r-mmio-audit.txt` is the Linux audit; the Mac regenerates it from its own image. The package entry is `ios-diag-t3r`, the hekate name `iOS Diag T3R`.

## Mac record (owner's report, 2026-11-27 13:58 JST, commit `4a903d590`)

| Item | Result |
| --- | --- |
| `bazel build //boot:diag_t3r` | success |
| Regression tests | 7 of 7 pass |
| Audit | `AUDIT PASSED (T3-R)` |
| D0 loader inspection | passed |
| `diag_images` | 11 of 11 inspections passed |
| Toolchain | Homebrew clang 23.1.6 |
| Mac standalone flat SHA256 of `diag_t3r` | `033100581fb897ee319858a7174f17728581cdacd32560be2de699a763ebd072` |
| Ten earlier flat hashes | identical between `6da4f57ee` and `4a903d590` on the same Mac; the only difference is the added `diag_t3r` line (diff exit 1) |

The Mac flat hash differs from the Linux one (`fc904cfc...`) because the toolchains differ (clang 23.1.6 against Ubuntu clang 18.1.3), as for every other image. This is a build and static-check record. It says nothing about the console: no run, no card copy, `boot` 0, feature mask 0, T3 and T4 not waived, the first XNU run on HOLD. The Mac `loader.uboot.bin` SHA256 and the package file hashes are not in the report and are recorded when the package is built for a run.

## Mac record of the fixed image (owner's report, 2026-11-27 20:14 JST, commit `907ae9923`)

| Item | Result |
| --- | --- |
| Regression tests | 8 of 8 pass |
| Mac flat SHA256 of the fixed `diag_t3r` | not reported |
| Card hash comparison after the T3-R run | exit 0 |
| Existing inis | the five restored; six in total now |
| Diff after the restore | exit 1; the differences were not reported |

On 2026-11-28 the owner reported the Mac flat SHA256 of the fixed `diag_t3r` as `74731b78275545fe695e47e21a9e68266dd5894efa2c56e7cd14a4e9dc1d28ff` (commit `302788696`, clang 23.1.6); the eleven hashes matched `907ae9923`.

The Mac comparison of the eleven image hashes starts from this commit, `907ae9923`, not `6da4f57ee`, which has no fixed T3-R. The exit 1 is recorded as it came. Its cause is not known here and is not guessed: it can be an expected difference (the sixth ini) or something else, and only the diff text says which. The fixed image has had no run. The Linux flat hash of the fixed image is `075d6816...`.

## First run

Run once by the owner on 2026-11-27 (report 16:10 JST): reads 22, stop 0000, no writes, automatic return to hekate. Two defects of the image were found (the `DID NOT ADVANCE` flag and the overlapping summary lines) and `PWM_CSR_1` read `0x00000000`, not hekate's off word. Details and the separation of read success from display faults: `ERISTA_T3R_RESULT.md`. This run is not repeated. The display defects are fixed in the code and checked offline; the fixed image has no Mac check and no run, and the Mac record below is of the image that was run. GPIO and PMC reads at non-secure EL1 answered in that one run; the "unconfirmed" wording below is the state before it.

## Recovery rehearsal (owner's report, 2026-11-27 14:47 JST)

The owner decided to skip further rehearsal. What was reported, and what was not measured:

| Item | State |
| --- | --- |
| Time from POWER hold to fully off | not measured; the moment the console is off is hard to tell from outside |
| hekate start time | about 4 to 5 s (owner's estimate) |
| Auto Boot | confirmed off |
| RCM entry, payload injection, manual recovery after the D0-S v1 freeze | succeeded in the past (D0-S v1 freeze and recovery, earlier runs); not repeated for this run |
| Time from injection to the menu | not measured |
| POWER hold from a console hung inside a read | not tried and not to be tried |

Unmeasured items stay unmeasured. The 26 s expectation and the 60 s cap in this document remain provisional assumptions. This record does not guarantee recovery from a hang: a read that never completes may not be left by POWER hold, and the past recovery after the D0-S v1 freeze is evidence about that one case, not a promise for this image. If the screen stops on a BEGIN line, the person holds POWER, and if that fails, uses the RCM jig; neither is asserted to work.

## T3-R card preparation (not executed; copying stays on hold)

Nothing is copied until the owner approves a run in writing. These steps are written so the card can be prepared and checked the same way as for T4-R, and the way back is kept. The package is `bazel-bin/boot/diag_t3r/package/` (the path the build prints). Its files are `switchroot/ios-diag-t3r/{bl31.bin,bl33.bin,script.txt}` and `bootloader/ini/ios-diag-t3r.ini`. Set `CARD=/Volumes/"NO NAME"` and `PKG=<that path>`. Every step before the copy only reads.

1. Hash the package files and keep the list with the run record: `cd "$PKG" && find . -type f | sort | xargs shasum -a 256`.
2. Record the card: `cd "$CARD" && find bootloader switchroot -type f | sort | xargs shasum -a 256 > ~/t3r-before.txt`. This file is the way back.
3. Collision check: `ls "$CARD"/bootloader/ini/ios-diag-t3r.ini "$CARD"/switchroot/ios-diag-t3r 2>&1` must report that neither exists, and `grep -ril t3r "$CARD"/bootloader/ini` must print nothing. If an earlier copy exists, compare its hashes with step 1 before replacing anything.
4. Menu: the owner decides whether the T3-R entry stands alone. To do that, make `"$CARD"/_ini_stash` outside `bootloader`, `mv` each diagnostic ini into it one by one (the T4-R ini included; no ini of a normal iOS package may be on the card), and compare `shasum -a 256 "$CARD"/_ini_stash/*` with the same lines of `~/t3r-before.txt` before going on. Nothing is deleted and no ini is renamed in place.
5. Copy only the four package paths without replacing anything: `rsync -rtv --ignore-existing "$PKG"/ "$CARD"/`.
6. `sync`, eject, re-insert, then `cd "$CARD" && find bootloader switchroot _ini_stash -type f 2>/dev/null | sort | xargs shasum -a 256 > ~/t3r-after.txt`.
7. Compare: without a stash `diff ~/t3r-before.txt ~/t3r-after.txt` shows exactly four added lines equal to the package hashes; with a stash the moved inis keep their hashes and nothing else differs.
8. After the run, or if it is never made: `mv` the inis back, remove the empty `_ini_stash` (the only remove), repeat step 6, and check the result is `~/t3r-before.txt` plus the four T3-R lines.

The card must not hold a root partition for XNU or an `ios.ini` that boots iOS during this run. Whether hekate ignores an unknown folder is looked at in the menu after the copy, as for T4-R.

## Before a run can be approved

All of these, in writing, from the owner:
- The Mac build and checks of the commit to be used (the record above covers `4a903d590`; a later commit needs its own).
- The owner's recovery position above (rehearsal skipped, figures unmeasured), accepted in writing for this run.
- The card procedure run through step 7 with the diff as expected.
- A stated reading of the 30 s and 60 s rules, and who holds the console.

Record per run: commit, `loader.uboot.bin` SHA256, package hashes, video file, the last line on screen, the stop code (`0x901` to `0x904` or 0), the 22 raw words, the card diff after the run. A run is not approved by this document.
