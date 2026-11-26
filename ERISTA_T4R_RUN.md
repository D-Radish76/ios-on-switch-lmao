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

## If it is ever run

Only after a separate written approval, with the common conditions of `ERISTA_T2_RUN.md` (cold start, temperature written down, a person with a hand on POWER). Pass: five reads return, `TMRUS` advances, the state line is drawn, the console returns to hekate by itself. Stop: the screen stops on a BEGIN line (hold POWER, report the register named on it), any red line, or no return to hekate. There is nothing to clean up: nothing was armed.
