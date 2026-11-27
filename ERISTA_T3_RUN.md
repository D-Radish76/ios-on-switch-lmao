# T3: build, check and (later) run sheet

`//boot:diag_t3` is built and tested offline. It is **not approved to run**: no hardware run, nothing on a card, `boot` stays 0, the feature mask stays 0, T3 and T4 are not waived, the first XNU run is on HOLD. Design, safety judgement and open points: `ERISTA_T3_FAN_DESIGN.md`.

## What the image does

D0's loader, then 20 allowlisted 32 bit reads, a state check, and only if every register matches, six reviewed writes of three registers: A5 high on `0x6000D020`, duty 51 on `0x7000A010`, the pad route on `0x7000320C`, and the three words that undo them. Every write is read back. The fan runs for about 5 s and the tach pad is polled in five windows. It ends the way every diagnostic does: the shared exit, a 15 s dwell, then PSCI `SYSTEM_RESET`.

Duty 51 is the only duty. The off sequence runs after every ending that followed a write, continues past a readback that differs, and is the only thing that puts the registers back. A reset is not claimed to do it.

The rail has three states in the code: off confirmed (read as off at entry, or read back as zero after the off write), maybe on (the on write was sent, or the off readback still shows A5 high) and unknown (the off readback shows something else). Nothing is called off before that readback. A failed rail off readback is sent again twice, after the other off writes ran. If it is still not confirmed, the run ends with `0A0B`, a red line, and no reset: the 15 s dwell and `SYSTEM_RESET` are skipped and the loader halts.

## Stop codes

| Code | Meaning | Write made? |
| --- | --- | --- |
| `0000` | pass: more tach changes at duty 51 than before | yes, then undone |
| `0A01` | the allowlist refused an access (a loader bug) | no |
| `0A02` | a line was not drawn and settled | no write after it; the off sequence if a write was made |
| `0A03` | the state reads ran past 20 s | no |
| `0A04` | a state word read as all ones | no |
| `0A05` | the state differs from the one the writes assume; the screen names the first difference | no |
| `0A06` | a write did not read back as written | the off sequence follows |
| `0A07` | the rail limit of 15 s was spent, checked on every tach pass, after every drawn line and before every on write | no on write after it; the off sequence follows |
| `0A08` | no pass: the spin window did not show more changes than the two before it | undone |
| `0A09` | a window held fewer than 1000 samples | undone |
| `0A0A` | a window ended by its sample limit, not its clock | the off sequence follows |
| `0A0B` | the rail was not read back as off after the off write and two sends more | the off sequence ran; no reset follows |

## Before the person starts it

The rail carries the fan and is shared. Detach both Joy-Con and every USB device; the console is on its battery with nothing on USB. The fan is not covered or blocked. These are not a safety guarantee: the fan current and the other loads on the A5 rail are not known.

## Person's rules for the run

- The notes are drawn first. If no new line appears for 30 s, hold POWER. A hung bus access is not left by any timeout in the image, and recovery from that state with the rail on is untested.
- Time from selecting the entry to hekate's menu: about 35 s expected, 60 s cap. Both are provisional and none is measured on the console. At 60 s without hekate, hold POWER. The rail limit of 15 s is a software limit on the counter. If a bus access hangs, it does not run, and neither does any other limit in this image.
- After COMPLETE the image waits 15 s and resets; hekate returns by itself. The rail is off by then, confirmed by a readback.
- If a red line says `RAIL OFF NOT CONFIRMED`, the image did not reset on purpose. The rail may be on and the fan may still be powered. Hold POWER at once and keep holding until the console is off, with no release for a photograph. Do not start any image again before the owner has read the screen photograph and the stop code `0A0B`. That the console switches off under POWER hold with the rail in this state is not shown by any measurement.
- If the console shows an exception line, do not rerun. Photograph it.

## Offline checks

```
bazel build --config=ios17 //boot:diag_t3
bazel test --config=ios17 //boot/diag/loader/test:t3_test //boot/diag/loader/test:t3r_test //tools/package/test:diag_t3_test //tools/package/test:diag_t3r_test //tools/package/test:diag_t4r_test //tools/package/test:diag_loader_test //tools/package/test:diag_mmio_audit_test //tools/package/test:erista_t3_fan_design_test
bazel run //tools/package:diag_images
```

The audit must end with `AUDIT PASSED (T3)`; `boot/diag/erista-t3-mmio-audit.txt` is the Linux one. `diag_images` must report 12 inspections. The eleven earlier hashes, `diag_t3r` (`075d6816...` on Linux) included, must be unchanged. Compare them on the Mac against commit `907ae9923`, the first commit with the fixed T3-R; `6da4f57ee` predates the fix and is not a baseline for `diag_t3r`. The package entry is `ios-diag-t3`, the hekate name `iOS Diag T3`. The Linux flat hash of `diag_t3` is in `boot/diag/erista-diag-image-hashes.txt`; the Mac's differs by toolchain.

## Card preparation (not executed; copying stays on hold)

Nothing is copied until the owner approves a run in writing. The procedure is `ERISTA_T3R_RUN.md`'s with `t3` for `t3r`: package `bazel-bin/boot/diag_t3/package/`, files `switchroot/ios-diag-t3/{bl31.bin,bl33.bin,script.txt}` and `bootloader/ini/ios-diag-t3.ini`, before and after hash lists `~/t3-before.txt` and `~/t3-after.txt`, a collision check, the optional ini stash, `rsync --ignore-existing`, the diff, and the way back. The last T3-R card diff ended with exit 1 and its text was not reported (`ERISTA_T3R_RUN.md`); read that text before a card is prepared again.

## Before a run can be approved

All of these, in writing, from the owner:
- The Mac build and checks of the commit to be used.
- The safety judgement in `ERISTA_T3_FAN_DESIGN.md` accepted, with CC4, rail loads and fan current named as unknown.
- The recovery position of `ERISTA_T3R_RUN.md` (rehearsal skipped, figures unmeasured), accepted for this run, and for a hang with the rail on in particular.
- The card procedure run through the diff with the expected result.
- What is attached to the console, and who holds it.

Record per run: commit, `loader.uboot.bin` SHA256, package hashes, video file, the last line on screen, the stop code, the six readback words, the five window counts, the card diff after the run. A run is not approved by this document.

## Mac record (owner's report, 2026-11-27 20:41 JST, commit `ce85b1572`)

Regression tests 10 of 10 pass. Build, audit and hash lines were not reported in that message. The code review that followed produced the second-review changes listed in `ERISTA_T3_FAN_DESIGN.md`; that commit has no Mac check yet.
