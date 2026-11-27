# T4-R: build, check and (later) run sheet

`//boot:diag_t4r` is built and tested offline. It is **not approved to run**: no hardware run, `boot` stays 0, the feature mask stays 0, T3 and T4 are not waived. What it reads and why that much: `ERISTA_T4R_DESIGN.md`.

## What the image does

D0's loader, then four allowlisted reads, each as a 32 bit word, each drawn on the screen and settled before it is made, with a RESULT line after it:

| Order | Register | Address |
| --- | --- | --- |
| 1 | `TMRUS` | `0x60005010` |
| 2 | `TMR5_PTV` | `0x60005060` |
| 3 | WDT0 config | `0x60005100` |
| 4 | WDT0 status | `0x60005104` |
| 5 | `TMRUS` again | `0x60005010` |

Between the two `TMRUS` reads the screen settles run, so the second value should be later by well over 100 ms worth of microseconds. A second value that is not later is drawn in red.

The three WDT0 words are classified. Status bits 4 to 11 are the current count and bits 12 to 13 the expiry count (mainline Linux `tegra_wdt.c`, for WDT0); both are drawn on their own line. Status bits 0 (enabled) and 1 (interrupt), and `PTV` bit 31 (enable), are the heartbeat code's names. Bits 0 and 1 have never been measured on this console, and WDT0 having WDT4's layout is an assumption, not a reading: the line says `BITS 0 AND 1 NOT CONFIRMED`. The raw words stay on the screen.

| State | Words |
| --- | --- |
| IDLE | timer stopped with no value, status clear, config 0 |
| CONFIGURED, STOPPED | the same with a config that is not 0 |
| DORMANT | status bit 0 set, timer stopped, config source 5 |
| BUSY | `TMR5_PTV` bit 31 set with a value (the count and expiry shown are read as they are), or status bit 1 set |
| UNKNOWN, with its reason | a word of all ones; a status bit that no source defines (2, 3, 14 to 31); `PTV` enabled with no value; a stopped timer holding a value; a stopped timer with a count left, with an expiry count left, or with both (three reasons); bit 0 set with a source other than 5 |

Nothing is stopped, set or cleared, whatever the words are. The image has no store to a device address. It ends the way every diagnostic does: the shared exit, a timed PSCI `SYSTEM_RESET` back to hekate.

## What a result does not show

- **A read that hangs.** If a register does not answer, the bus can hang and no software timeout leaves it (D0-S v1's hang is the precedent). The screen stops on the BEGIN line of that register. The person holds POWER. The BEGIN line is drawn first, so the photograph names the register.
- **The state right after a reset.** The state shown is the state after hekate's init and after hekate launched the loader. It says nothing about the state in the first moments after a reset, which hekate has already run over.
- **A watchdog reset.** `TMRUS` counting from a recent start and the return to hekate are not direct evidence that a watchdog ended any run. `PMC_RST_STATUS` cannot help: hekate clears it before the loader runs.
- **That WDT0 works.** A read says the register answers. It says nothing about a write or an expiry.

## Offline checks

From the repository root, on Linux or on the Mac:

```
python3 tools/package/diag_images.py                       # the ten images; the nine earlier hashes must not change
python3 tools/lint/runfiles_check.py diag_t4r_test
python3 tools/lint/runfiles_check.py diag_first_xnu_docs_test
```

The Mac, with Bazel:

```
bazel build --config=ios17 //boot:diag_t4r
shasum -a 256 bazel-bin/boot/diag_t4r/loader.uboot.bin bazel-bin/boot/diag_t4r/package/switchroot/ios-diag-t4r/*
cat bazel-bin/boot/diag_t4r/inspection.txt
bazel test --config=ios17 //tools/package/test:diag_t4r_test //boot/diag/loader/test:t4r_test \
  //tools/package/test:diag_state_v2_test //tools/package/test:diag_t2_test //tools/package/test:diag_mmio_audit_test \
  //tools/package/test:diag_first_xnu_docs_test
python3 tools/package/diag_mmio_audit.py --disassembly bazel-bin/boot/diag_t4r/loader.dis --uboot-image bazel-bin/boot/diag_t4r/loader.uboot.bin --t4r
```

The audit must end with `AUDIT PASSED (T4-R)`. The Linux flat hash is `de467deb...` after the 2026-11-27 revision (`boot/diag/erista-diag-image-hashes.txt`; `9418d085...` before it); the Mac's `loader.uboot.bin` is a different file, as for every other image, and is recorded after the Mac build. The package entry is `ios-diag-t4r`, the hekate name `iOS Diag T4R`: it does not collide with the normal `ios` entry or with any other diagnostic.

## Mac record of the revised build (reported 2026-11-27 10:10 JST, commit `f27f19d42`)

The Mac repeated the build, the regression tests and the audit on the revised image.

| Item | Result |
| --- | --- |
| `bazel build --config=ios17 //boot:diag_t4r` | success |
| Mac `loader.uboot.bin` SHA256 | `4116a2b2082331ff8ac8d9e8aecb2f52491d39d8a3e2a41fdc888cdf7a89bc17` |
| Mac LLVM 23.1.6 flat `diag_t4r` SHA256 | `13338028a1bb3baddda8dc79ca8f03036d391448a41b110798389da3a25be99b` |
| Regression tests | 9 of 9 pass, the real gtest included |
| Audit | `AUDIT PASSED (T4-R)` |
| Inspection | 10 of 10 passed |
| The nine earlier flat hashes | identical to the previous Mac measurement, 64 digits each (the table below); only `diag_t4r` changed |

Package files (Mac Bazel build):

| File | SHA256 |
| --- | --- |
| `bootloader/ini/ios-diag-t4r.ini` | `d83af011684668de10ddfd8f41909e1592f227f13db88f3d58f0e12b57f5fbd1` |
| `switchroot/ios-diag-t4r/bl31.bin` | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` |
| `switchroot/ios-diag-t4r/bl33.bin` | `ea89ffba1b256cbd3cc530857d26d895baf439803cfc75f54835aef06fbdcbcb` |
| `switchroot/ios-diag-t4r/script.txt` | `f762d2a30ce57ee1a4f38a96f7e2c2bc8e347669814dedb07d615dec48da01ef` |

This is the build a run would use. The first-build section after this one is kept as history; its `diag_t4r` values are superseded. The table in it for the other nine images still holds.

The "nine unchanged" statement rests on comparing two Mac measurements of the same toolchain. The comparison against the commit before T4-R (`a3275ff5f`, commands below) is still the stronger check and can be run at any time. It was not part of this report.

## Mac build record of the first build (reported 2026-11-27 03:41 JST, commit `d3aea29e7`)

The T4-R image changed afterwards (the classification and the screen lines, below), so the Mac `loader.uboot.bin` and `diag_t4r` flat hashes in this section describe the first build only. The other nine flat hashes did not change. The revised build is to be run again on the Mac.

| Item | Result |
| --- | --- |
| `bazel build --config=ios17 //boot:diag_t4r` (the first build, `d3aea29e7`; superseded by the revision below) | success, 37 actions, D0 loader inspection passed |
| Mac `loader.uboot.bin` SHA256 | `a7a345250212f8ff2fa217919a6feaa966d25fdb8a7cd9e7ed934d71a9d515b0` |
| Tests | 6 of 6: `diag_t4r_test`, `t4r_test` (the real gtest), `diag_state_v2_test`, `diag_t2_test`, `diag_mmio_audit_test`, `diag_first_xnu_docs_test` |
| Audit | `AUDIT PASSED (T4-R)`: `0x60005010`, `0x60005060`, `0x60005100`, `0x60005104` once each; `switch_t4r_access` the only caller of the word load; no device store |

The Mac had no `ld.lld`; Homebrew `llvm` and `lld` 23.1.6 (clang, lld, `llvm-objcopy`, `llvm-objdump`) were installed, and `diag_images.py` passed 10 of 10. Flat image SHA256 under that toolchain. It is a different toolchain from the Linux one in `boot/diag/erista-diag-image-hashes.txt`, so the values differ and the two sets are never compared with each other:

| Image | Mac LLVM 23.1.6 flat SHA256 |
| --- | --- |
| `diag_loader` | `77a90464dba0e24fbe2f6472e76576048b82aee2412bd86bcac04dd6ac19179d` |
| `diag_loader_r` | `fe918973d093e9d49898fdf15de2b51dc9950b2cbe2af251c866a089775c40e8` |
| `diag_loader_sd` | `ec637dda8add8738aa796e4e3c2ab23a8635ac6210a3f61735a1bf41ae9a2329` |
| `diag_t1` | `9cb56e79bf8f21418a6272f3aadcd51da3e40473de56e29bd8917e4e8cf17d11` |
| `diag_t1r` | `2b08bab8d274a4cd50421929367c49c6b0fb049b8181cbab33a5b449fc97b1ae` |
| `diag_t1v3` | `c64b1518b67b9b5d0ff2ba078ab317aeea99dfc37fd2351b9d51e72e6640d2e4` |
| `diag_t1t` | `277f57a8e6105fa501c369030000c6a237ca0c493e6392531e98c3f9695eb2a8` |
| `diag_t2` | `75135794988a164b59c815b6046fd926ab4e9855d03aba535f2d235524932c91` |
| `diag_state_v2` | `47c8e9fe9ef734eab9f95bf8bdec6b88f1e73c9912dc0078115943bb254e5012` |
| `diag_t4r` | `d455378805f24c5ce15a09a8b57bbff8a5dae4db0b4b591aec88deef3240b772` |

Not shown by these numbers: that the nine earlier images are unchanged on the Mac. The Linux record shows it for the Linux toolchain. To show it for this toolchain, build the commit before `diag_t4r` with the same LLVM and compare:

```
git worktree add ../pre-t4r a3275ff5f
(cd ../pre-t4r && python3 tools/package/diag_images.py) > /tmp/pre.txt
python3 tools/package/diag_images.py > /tmp/post.txt
diff <(grep -v diag_t4r /tmp/post.txt) /tmp/pre.txt && echo nine unchanged
git worktree remove ../pre-t4r
```

## What the audit and the tests cannot show

The audit reads the linked image and its disassembly. It shows which device words the image holds, which function loads from them and who calls it. It does not show that no other code path could form such an address at run time from data, that the compiler kept the read order, or what the hardware does on a read. The tests model the logic with a fake register file; the real bus is not in them. A read of a register that does not answer is not a case any test can reach. The classification rests on bit names whose sources differ in weight, and the 2026-11-27 revision of it is in `ERISTA_T4R_REVIEW.md`.

## Every file of the package

The package is `bazel-bin/boot/diag_t4r/package/` (the exact path is the one the build prints). Its files are `switchroot/ios-diag-t4r/{bl31.bin,bl33.bin,script.txt}` and `bootloader/ini/ios-diag-t4r.ini`. Hash every file, and keep the list with the run record:

```
cd bazel-bin/boot/diag_t4r/package && find . -type f | sort | xargs shasum -a 256
```

## Before anything is put on a card

Nothing is copied; copying stays on hold. These are the checks to make first, all read-only:

```
find /Volumes/<card> -maxdepth 3 \( -path '*/switchroot/*' -o -path '*/bootloader/ini/*' \) | sort
ls -la /Volumes/<card>/bootloader/ini /Volumes/<card>/switchroot
```

- The package's four paths use the entry name `ios-diag-t4r`. None is a path of a normal iOS package (`ios.ini`, `switchroot/ios/`), of the first-XNU packages, or of another diagnostic (`ios-diag-loader`, `ios-diag-state-v2`, and the rest). If the card already holds `ios-diag-t4r`, it is an earlier copy of this image: compare its hashes with the list above before replacing anything.
- The ini names `bootloader/res/icon_ios_hue.bmp` but the package does not ship it. A card with no icon shows hekate's default; that is not a collision and not an error.
- Take the hashes of the card's `switchroot` and `bootloader/ini` before and after the copy and after a re-insert, as `ERISTA_FIRST_XNU_RUN.md` rows 5 and 6 do for the first run.

## Stop and pass conditions, final (revised 2026-11-27)

The earlier text said hekate must return within 15 s of the COMPLETE line. It cannot: the 15 s wait starts after COMPLETE, not at it. From `boot/diag/loader/diag.c` and `exit.c`:

1. The five reads and their lines take 14 shown steps of about 100 ms each, so about 1.5 s plus drawing. The last is `T4R COMPLETE`. A step that does not settle ends the run.
2. `switch_diag_exit` draws `RESET IN 15 S` after COMPLETE (a few draws later, well under 5 s).
3. It sets a deadline 15 s after that line on the CPU counter (`SWITCH_DIAG_WAIT_SECONDS`, 19.2 MHz) and waits with `wfe`. The screen does not change during these 15 s.
4. At the deadline it makes one PSCI `SYSTEM_RESET` call. The reset is therefore issued about 15 s plus the draw time after COMPLETE.
5. If the call returns (it never did in D0, D0-R, T1-T, T1 v3, D0-S v2, D1-min and T2), the loader draws `PSCI SYSTEM_RESET RETURNED <x0>. HOLD THE POWER BUTTON` in red and halts. If the counter is not as expected or the core is not at EL1, it draws that and halts without a reset.
6. Hekate's start after the reset is not timed on this path. The only figure in the repository is the D0 video: about 21 s in all for a 15 s wait, which leaves about 6 s from reset to hekate's menu (one video, a U-Boot reset, not PSCI).

So hekate is expected about 22 s after COMPLETE. `ERISTA_T2_RUN.md` estimates about 40 s from the first U-Boot line to hekate and keeps 50 s from selecting the entry as the provisional cap for a longer stage than this one. The conditions below are consistent with all of that. Every figure is provisional. A time limit is not a safety guarantee: it tells the person when to act, and it does not stop a hang, a hot SoC or a bus fault on its own.

Common conditions of a run, if one is ever approved in writing: those of `ERISTA_T2_RUN.md` (cold start, temperature written down and under the provisional 40 C, video on, a person with a hand on POWER, the card holding only this entry).

| | Condition |
| --- | --- |
| Pass | all five reads return; `TMRUS` advances; the state and the count and expiry lines are drawn; `T4R COMPLETE. READS 5. STOP 0000`; `RESET IN 15 S`; hekate comes back by itself |
| Hold POWER | the screen stops on a BEGIN line (photograph, report the register named on it); no screen change for 30 s before COMPLETE; `RESET IN 15 S` not drawn within 5 s of COMPLETE; hekate not back 30 s after COMPLETE (the 15 s wait and a 15 s margin for hekate); 50 s since selecting the entry, whichever of the last two comes first; `PSCI SYSTEM_RESET RETURNED`, `NO RESET`, `EXIT CALLED OUTSIDE EL1`, an exception, a black screen or garbage; the console feels hot |
| Report, no stop | `DID NOT ADVANCE`; `STOP` not 0000 (`0901` a refused read, `0902` a line that did not settle); any state, UNKNOWN included, with its reason |
| What a pass does not show | that a write works, that a watchdog resets the console, anything about the state right after a reset, or anything about XNU |

A static screen is normal for the 15 s between `RESET IN 15 S` and the reset, so the 30 s no-change rule applies before COMPLETE only. BUSY, DORMANT, CONFIGURED and UNKNOWN are results: nothing is touched whatever they are. Nothing was armed, so there is nothing to clean up, and the return to hekate carries no loop risk from this stage.

## Mac final check of `18bc5871f` and the card as found (reported 2026-11-27 10:24 JST)

| Item | Result |
| --- | --- |
| Docs test `diag_first_xnu_docs_test` | PASS |
| `tools/lint/duplicate_labels.py` | `no label is listed twice` |
| Nine earlier images, Mac LLVM, commit before T4-R (`a3275ff5f`, a detached worktree) against the current commit without `diag_t4r` | no difference, comparison exit 0 |

Card as found (read only, nothing written): device `disk10s1`, volume `NO NAME`, FAT32, 32.0 GB, 23.3 GB free, partition offset 8192 sectors, 512 byte blocks, not read-only. `bootloader/ini` and `switchroot` hold no T4-R file and no ini refers to T4-R. The active diagnostic inis are `ios-diag-loader.ini`, `ios-diag-t1t.ini`, `ios-diag-t1v3.ini` and `ios-diag-t2.ini`. They stay on the card; none is deleted or edited.

## Host side verification is complete when

All of these hold for the commit a run would use: the Mac build, 9 of 9 regression tests, the real gtest, `AUDIT PASSED (T4-R)`, 10 of 10 inspection, the nine earlier images identical to the commit before T4-R, the package hashes above, the docs test and the duplicate label lint. All were met at `18bc5871f`. It does not show anything the audit and tests cannot show (see that section): the real bus, a read that hangs, the meaning of status bits 0 and 1.

## Card procedure (not executed; copying stays on hold)

Set `CARD=/Volumes/"NO NAME"` and `PKG=<the package path the build prints>`. Every step before the copy only reads.

1. Record the card before anything: `cd "$CARD" && find bootloader switchroot -type f | sort | xargs shasum -a 256 > ~/t4r-before.txt`. Keep the file; it is the way back.
2. Collision check: `ls "$CARD"/bootloader/ini/ios-diag-t4r.ini "$CARD"/switchroot/ios-diag-t4r 2>&1` must report that neither exists, and `grep -ril t4r "$CARD"/bootloader/ini` must print nothing.
3. Package check: `cd "$PKG" && find . -type f | sort | xargs shasum -a 256` must equal the four hashes in this file.
4. Setting the four existing inis aside, if the owner wants the T4-R entry alone in the menu. Nothing is deleted. Make a stash on the card outside `bootloader`, `mkdir "$CARD"/_ini_stash`, then `mv` each of the four inis into it one by one. Take a record of the stash (`shasum -a 256 "$CARD"/_ini_stash/*`) and compare it with the same four lines of `~/t4r-before.txt`; they must be equal before step 5. The `switchroot` folders of those diagnostics are not touched. Whether hekate ignores a folder it does not know has not been checked here; look at the menu after the copy. Do not rename the inis in place: hekate's rule for the extension has not been checked.
5. Copy only the four package paths with their relative paths: `rsync -rtv --ignore-existing "$PKG"/ "$CARD"/` (or `cp -n`). The flags stop it from replacing anything.
6. `sync`, eject, remove, re-insert, wait for the mount, then `cd "$CARD" && find bootloader switchroot _ini_stash -type f 2>/dev/null | sort | xargs shasum -a 256 > ~/t4r-after.txt`.
7. Compare. Without a stash, `diff ~/t4r-before.txt ~/t4r-after.txt` must show exactly the four added lines, each equal to its package hash. With a stash, the four inis show as moved (same hash, new path) and nothing else differs.
8. After the run, or if the run is never made: `mv` the four inis back from `_ini_stash`, remove the empty `_ini_stash` folder (the only remove), repeat step 6 and check the result is `~/t4r-before.txt` plus the four T4-R lines. The T4-R files stay unless the owner says otherwise.

Any difference not named above stops the procedure and is reported with both files.

## Card placement as measured (reported 2026-11-27 10:29 JST, commit `7424abcc4`)

The procedure above was run on the Mac by the owner. It is the first time the T4-R files are on the card. Nothing was run on the console.

| Step | Result |
| --- | --- |
| Hash record before the copy | 78 files |
| Package files against this file's list | 4 of 4 equal, 64 digits each |
| Four existing inis moved to `_ini_stash` | done; stash comparison exit 0 (same hashes as before) |
| `rsync` dry run | only the four planned files |
| `rsync` copy | success; package size 1004732 bytes |
| Before and after diff | four inis moved, four T4-R files added, 0 changed hashes, no other difference |

State of the card now: the four diagnostic inis sit in `_ini_stash`, so hekate's menu should show `iOS Diag T4R` and no other diagnostic. Not yet looked at: the menu itself, and whether hekate ignores `_ini_stash`. The inis are to be moved back after the run (step 8 of the procedure). Until then the other diagnostics cannot be started from the card.

Having the files on the card is not a run approval. The run is not approved.

## Timing reported after a run (2026-11-27 10:37 JST, owner's impression, not measured)

The owner reported: hekate came back by itself about 15 s after the COMPLETE line; that is inside the provisional 30 s stop figure; no manual stop; no second run. The 15 s is the owner's impression, not a measurement from the video, and the report gives no screen values, so no state, count or register value is recorded here and none is inferred.

How to read the figure: it is a reference, not the PSCI `SYSTEM_RESET` time and not a result of this repository's measuring. It does not fit the exit path as read from `exit.c` without a further explanation. From COMPLETE the loader draws `RESET IN 15 S`, waits 15 s on the counter and only then calls `SYSTEM_RESET`, so hekate could not be back within 15 s of COMPLETE unless the 15 s was counted from another line (the `RESET IN 15 S` line, say), or from the reset and not from COMPLETE, or the impression is off. This record does not choose between them. The video, if one was taken, settles it: clock at COMPLETE, at `RESET IN 15 S`, at the screen going dark and at the hekate menu.

Until that is read from the video, the figures in "Stop and pass conditions, final" are not changed: the 22 s estimate and the 30 s stop stay as provisional, and no time limit is a safety guarantee. The pass conditions are not claimed either; they need the screen lines of the template below.

## Checklist for a run, if one is ever approved in writing

Everything here is a precondition. One "no" means no run.

Written approval and state:
- [ ] The owner's written approval of the T4-R run exists, with the date.
- [ ] `boot` 0, feature mask 0; nothing else of the repository is on the card's boot path; no watchdog is armed by anything on the card.

Card:
- [ ] A fresh `shasum` record of `bootloader` and `switchroot` equals `~/t4r-after.txt`.
- [ ] The four T4-R files equal the hash list in this file.
- [ ] The card holds the entry `ios-diag-t4r` and no other entry that boots iOS; the four inis are in `_ini_stash`.

Console and set-up:
- [ ] Cold start: powered off for several minutes, not charging, not just used.
- [ ] Temperature of the case written down, under the provisional 40 C (not an official limit).
- [ ] Video on, framing the screen and the clock or a timer.
- [ ] A person with a hand on POWER for the whole run.
- [ ] Hekate 6.5.5 starts and shows `iOS Diag T4R`, one entry. Note whether `_ini_stash` caused anything in the menu.

During (write down each line as it is drawn; the screen is the record):
- [ ] Title, `T4R START. EL1 STATE PASS`, then each BEGIN and RESULT pair for `TMRUS 1`, `TMR5 PTV`, `WDT0 CONFIG`, `WDT0 STATUS`, `TMRUS 2`.
- [ ] The count and expiry line, the state line with its reason, `T4R COMPLETE. READS 5. STOP 0000`, `RESET IN 15 S`.
- [ ] Hold POWER on any condition in "Stop and pass conditions, final".

After:
- [ ] Hekate came back by itself; time from COMPLETE to the menu.
- [ ] A new hash record of the card; the diff against `~/t4r-after.txt` is empty.
- [ ] The four inis moved back and the card equals `~/t4r-before.txt` plus the four T4-R files.

## Record template for after a run

Copy it into the run record. Every blank is filled from the video or the screen; a blank that cannot be read stays blank and is written as "not seen", never estimated.

```
Run date and time (JST):
Commit and package hashes checked (yes/no):
Case temperature before / after:
Entry selected at (clock):
Screen lines, in order, as drawn:
  TMRUS 1 address / value:
  TMR5 PTV address / value:
  WDT0 CONFIG address / value:
  WDT0 STATUS address / value:
  TMRUS 2 address / value / advanced by (or DID NOT ADVANCE):
  Count / expiry line:
  State and reason (BUSY, DORMANT, IDLE, CONFIGURED_STOPPED, UNKNOWN):
  COMPLETE line (reads, stop code):
  RESET IN 15 S drawn (yes/no) and clock:
Hekate back by itself (yes/no), clock, seconds after COMPLETE:
Stopped by hand (yes/no), why, at which line:
PSCI SYSTEM_RESET RETURNED / NO RESET / EXIT CALLED OUTSIDE EL1 / exception / black screen:
Console warm or hot (observation):
Card hash diff after the run (empty/not):
Inis moved back (yes/no) and card equals the before record plus four files (yes/no):
Evidence the run adds (one line each): what it shows / what it does not show
Video file name and its SHA256:
```

A pass shows what "Stop and pass conditions, final" says it shows and nothing more.

## Moving on to T4-A

T4-A is not implemented and not approved. It arms and expires a watchdog. These are the conditions the owner would check before asking for it, all after a real T4-R run:

- The run passed as defined above: five reads, STOP 0000, hekate back by itself, nothing on the card changed.
- The raw words and the state are recorded. If WDT0 read BUSY or UNKNOWN with a running count, T4-A does not start until that is explained.
- The three open items are answered or accepted in writing: WDT0 answered at EL1 without a hang; status bits 0 and 1 have a meaning for WDT0 (this run shows the values, it does not name the bits); the timer-number disagreement with L4T's DTB does not matter for the registers read.
- The measured time from COMPLETE to hekate is on record, so a T4-A time limit rests on a number.
- The owner approves T4-A separately: its writes, its allowlist, its recovery by POWER hold and cold start, and its loop risk. The T4-R pass approves none of that.
