# T1-R: clock diagnostic run sheet

Hardware boot is on hold. This sheet is for the run once it is approved. One run, no automatic retry, `boot` stays 0.

T1-R reads and shows values. It does no I2C transfer and writes no device register. Its result says where
`CLK_SOURCE_I2C1` holds hekate's value and where it does not. It does not show that TMP451 answers, any temperature, or any
SCL frequency. The 50 s total is a point for the person to act, not a thermal limit.

## 1. Build and verify (Mac)

```
bazel test //boot/diag/loader/test:t1r_test //tools/package/test:diag_t1r_test //tools/package/test:diag_loader_test //tools/package/test:diag_t1_test
bazel build //boot:diag_t1r
PKG="$PWD/bazel-bin/boot/diag_t1r/package"
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256)
```

Record the four hashes (`bl31.bin`, `bl33.bin`, `script.txt`, the ini) in the review before copying. The D0, D0-R, D1 and T1
v2 packages must hash as before: build them and compare with the hashes already recorded.

## 2. Copy and check (Mac)

```
export COPYFILE_DISABLE=1
SD="/Volumes/CARD"
mkdir -p "$SD/switchroot/ios-diag-t1r"
cp "$PKG"/switchroot/ios-diag-t1r/bl31.bin "$PKG"/switchroot/ios-diag-t1r/bl33.bin "$PKG"/switchroot/ios-diag-t1r/script.txt "$SD/switchroot/ios-diag-t1r/"
cp "$PKG"/bootloader/ini/ios-diag-t1r.ini "$SD/bootloader/ini/"
sync
cd "$SD" && shasum -a 256 switchroot/ios-diag-t1r/* bootloader/ini/ios-diag-t1r.ini
```

The four hashes equal the recorded ones. Eject, insert again, run the last line again.

## 3. Before starting

Same as `ERISTA_T1_RUN.md` section 3, with the entry `iOS Diag T1R` and a 50 s deadline from selecting it. Film the screen
from the first U-Boot line: P1 and P2 exist only in the video.

## 4. Expected screen

| Point | Where | What is shown |
| --- | --- | --- |
| P1 | U-Boot, label `T1R P1. BEFORE BOOTM START` | five `md.l` lines: 60006124, 60006128, 60006004, 60006010, 600060a0 |
| P2 | U-Boot, label `T1R P2. AFTER BOOTM PREP`, then a 5 s pause | the same five, same order |
| P3 | loader, title `LOAD DIAG T1R`, label `T1R P3 LOADER AT EL1` | eleven lines, closing `T1R COMPLETE CODE 0000` |

No value is a pass or fail. T1 v2 measured at P3: `CLK_SOURCE_I2C1` 00000003, `RST_L` 049102CA, `ENB_L` 9802D131. Hekate
writes `C0000003` for I2C1. The reading table is in `ERISTA_T1R_DESIGN.md`. After P3 the loader waits 15 s and resets.

## 5. Abort

Hold the power button and report the last line and its time if:

- 50 s have passed since the start and hekate has not returned;
- a stage passes the deadlines of `ERISTA_T1_RUN.md` section 5;
- the screen shows an exception, `T1R STOPPED CODE`, `NO RESET`, `PSCI SYSTEM_RESET RETURNED`, a black screen or garbage;
- the console feels hot.

A screen that stops changing is a stall. A hung register read cannot be ended by the loader; only the button does.

## 6. After the run

One run. If every point reads `C0000003`, do not retry: wait for review. After an abnormal end, power off, wait 10 s and follow
the Nyx recovery check in `ERISTA_T1_RUN.md` section 6.

Report the video, the selection-to-menu time, and the Nyx status-bar value.
