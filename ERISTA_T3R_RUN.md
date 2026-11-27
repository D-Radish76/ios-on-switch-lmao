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

The audit must end with `AUDIT PASSED (T3-R)`. The Linux flat hash is `fc904cfc...` (`boot/diag/erista-diag-image-hashes.txt`); the Mac's `loader.uboot.bin` is a different file and is recorded after the Mac build, as for every other image. `boot/diag/erista-t3r-mmio-audit.txt` is the Linux audit; the Mac regenerates it from its own image. The package entry is `ios-diag-t3r`, the hekate name `iOS Diag T3R`.

## Run sheet (not approved)

A run needs the owner's written approval, a Mac build of one commit, and the recovery rehearsal. Nothing here is a request to run. Record per run: commit, `loader.uboot.bin` SHA256, video file, the last line on screen, the stop code (`0x901` to `0x904` or 0), and the 22 raw words.
