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

The three WDT0 words are classified, by the heartbeat's bit names (status bit 0 enabled, bit 1 interrupt, `PTV` bit 31 enable) and on the assumption, not a reading, that WDT0's status has WDT4's layout:

| State | Words |
| --- | --- |
| IDLE | timer stopped with value 0, not enabled, config 0 |
| CONFIGURED, STOPPED | the same with a config that is not 0 |
| DORMANT | enabled, timer stopped, config source 5 |
| BUSY | timer 5 running, or an interrupt pending. Left alone |
| UNKNOWN | a word of all ones; status bits other than 0 and 1; a stopped timer with a value; enabled with a source other than 5. The reason is on the line |

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

The audit must end with `AUDIT PASSED (T4-R)`. The Linux flat hash is `9418d085...` (`boot/diag/erista-diag-image-hashes.txt`); the Mac's `loader.uboot.bin` is a different file, as for every other image, and is recorded after the Mac build. The package entry is `ios-diag-t4r`, the hekate name `iOS Diag T4R`: it does not collide with the normal `ios` entry or with any other diagnostic.

## Mac build record (reported 2026-11-27 03:41 JST, commit `d3aea29e7`)

| Item | Result |
| --- | --- |
| `bazel build --config=ios17 //boot:diag_t4r` | success, 37 actions, D0 loader inspection passed |
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

The audit reads the linked image and its disassembly. It shows which device words the image holds, which function loads from them and who calls it. It does not show that no other code path could form such an address at run time from data, that the compiler kept the read order, or what the hardware does on a read. The tests model the logic with a fake register file; the real bus is not in them. A read of a register that does not answer is not a case any test can reach. The classification rests on bit names whose sources differ in weight (`ERISTA_T4R_REVIEW.md`).

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

## Stop and pass conditions, final

These apply only if the owner approves a run in writing, with the common conditions of `ERISTA_T2_RUN.md` (cold start, temperature written down and under the provisional 40 C, video on, a person with a hand on POWER, the card holding only this entry). The 30 s and 15 s figures are provisional and are not a safety guarantee.

| | Condition |
| --- | --- |
| Pass | all five reads return; `TMRUS` advances (a line in white with `ADVANCED BY`); the state line is drawn; `T4R COMPLETE. READS 5. STOP 0000`; the console returns to hekate by itself |
| Stop, hold POWER | the screen stops on a BEGIN line (report the register named on it, photograph first); no screen change for 30 s; no return to hekate within 15 s of the COMPLETE line |
| Stop, report | `DID NOT ADVANCE`, `STOP` not 0000, a refused or not shown step (`0901`, `0902`) |
| What a pass does not show | that a write works, that a watchdog resets the console, anything about the state right after a reset, or anything about XNU |

A BUSY, DORMANT or CONFIGURED state is a result to report, not a stop: nothing is touched. UNKNOWN is reported with its reason and decides nothing by itself. Nothing was armed, so there is nothing to clean up, and the return to hekate carries no loop risk from this stage.
