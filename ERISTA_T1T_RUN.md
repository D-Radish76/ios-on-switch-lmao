# T1-T: timing register run sheet

Hardware boot is on hold until the final review. This sheet is for the run once it is approved. One run, no automatic retry,
`boot` stays 0.

T1-T reads and shows 25 registers. It starts no I2C transfer and has no store to a device address. Its words are candidates
for T1 v3's timing pins. They do not show that a value is a reset value or is safe. The 50 s total is a point for the person to
act, not a thermal limit.

## 1. Build and verify (Mac)

```
bazel test //boot/diag/loader/test:t1t_test //tools/package/test:diag_t1t_test //tools/package/test:diag_t1v3_test //tools/package/test:diag_loader_test
bazel build //boot:diag_t1t
bazel aquery //boot:diag_t1t --output=text | grep -c -- '--t1t'
PKG="$PWD/bazel-bin/boot/diag_t1t/package"
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256)
```

The `--t1t` count must be 1 or more. The same fact shows in `inspection.txt`: the inspection fails when `--t1t` is missing
(`the link has switch_t1t_run outside T1-T`), so "passed" means the T1-T checks ran.

The Mac build of commit `5d5468e74` gave these (reported by the user, 2026-11-26). They are the values to check on the card.

| File | SHA-256 |
| --- | --- |
| `switchroot/ios-diag-t1t/bl31.bin` | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` |
| `switchroot/ios-diag-t1t/bl33.bin` | `5629c9b39bf64f25e8c5a7dd4ed4cc5fdd310a28422ac7f209f00c89a5606088` |
| `switchroot/ios-diag-t1t/script.txt` | `f762d2a30ce57ee1a4f38a96f7e2c2bc8e347669814dedb07d615dec48da01ef` |
| `bootloader/ini/ios-diag-t1t.ini` | `a159985de40ea101cd3ab1683521e2c6eabc41af1113f7d4bd8250068b255d20` |

`bl31.bin` equals the other diagnostics'. The loader's flat image hash on the build host is `c81b53ad...14bc`
(`tools/package/diag_images.py`, clang 18.1.3); the Bazel clang may give another value, so it is not a card check.

## 2. Copy and check (Mac)

```
export COPYFILE_DISABLE=1
SD="/Volumes/CARD"
mkdir -p "$SD/switchroot/ios-diag-t1t"
cp "$PKG"/switchroot/ios-diag-t1t/bl31.bin "$PKG"/switchroot/ios-diag-t1t/bl33.bin "$PKG"/switchroot/ios-diag-t1t/script.txt "$SD/switchroot/ios-diag-t1t/"
cp "$PKG"/bootloader/ini/ios-diag-t1t.ini "$SD/bootloader/ini/"
sync
ls -la "$SD/switchroot/ios-diag-t1t" "$SD/bootloader/ini"
cd "$SD" && shasum -a 256 -c <<'SUMS'
dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4  switchroot/ios-diag-t1t/bl31.bin
5629c9b39bf64f25e8c5a7dd4ed4cc5fdd310a28422ac7f209f00c89a5606088  switchroot/ios-diag-t1t/bl33.bin
f762d2a30ce57ee1a4f38a96f7e2c2bc8e347669814dedb07d615dec48da01ef  switchroot/ios-diag-t1t/script.txt
a159985de40ea101cd3ab1683521e2c6eabc41af1113f7d4bd8250068b255d20  bootloader/ini/ios-diag-t1t.ini
SUMS
```

All four print `OK`, and `ls` shows no `._` files. Eject (`diskutil eject "$SD"`), insert again and run the check a second time.
Any `FAILED` or missing file: do not run. The older entries stay on the card untouched.

## 3. Before starting

Same as `ERISTA_T1_RUN.md` section 3 with the entry `iOS Diag T1T`, and a 50 s deadline from selecting it. Film the screen
from the first U-Boot line.

## 4. What was reviewed

| Item | Result |
| --- | --- |
| `--t1t` check | `loader.bzl` passes `--t1t` for `t1t = True`. `diag_loader.py link` run without it fails; with it, it passes. |
| Registers | 25 reads: 11 CAR, then 7 per controller. Table below. |
| I2C5 gate | read only if `CLK_OUT_ENB_H` bit 15 is set and `RST_DEVICES_H` bit 15 is clear. Bit numbers match hekate `CLK_H_I2C5 = 15`. I2C1 uses L bit 12 (`CLK_L_I2C1 = 12`). A block is judged on its own bits only. |
| Writes | none by construction. See section 7 for what the checks do not cover. |
| Screen | 16 lines at rows 9 to 24, footer at row 26, last row 34. Longest line 96 characters, 105 fit. |
| Time | see section 6 |

| Register | Address | Register | Address |
| --- | --- | --- | --- |
| `RST_DEVICES_L` | `60006004` | `CLK_SOURCE_I2C4` | `600063c4` |
| `RST_DEVICES_H` | `60006008` | `CLK_SOURCE_I2C5` | `60006128` |
| `CLK_OUT_ENB_L` | `60006010` | `CLK_SOURCE_I2C6` | `6000665c` |
| `CLK_OUT_ENB_H` | `60006014` | `PLLP_BASE` | `600060a0` |
| `CLK_SOURCE_I2C1` | `60006124` | I2C1 `CNFG` `STATUS` `INT_STATUS` `CLK_DIVISOR` `CONFIG_LOAD` `TIMING_0` `TIMING_1` | `7000c000` `+1c` `+68` `+6c` `+8c` `+94` `+98` |
| `CLK_SOURCE_I2C2` | `60006198` | I2C5, same offsets | `7000d000` + |
| `CLK_SOURCE_I2C3` | `600061b8` | | |

## 5. Expected screen

| Point | Where | What is shown |
| --- | --- | --- |
| U-Boot | the D0 lines | `BOARD OK`, `FRAMEBUFFER OK`, `DTB COPIED AND FRAMEBUFFER RESERVED`, `LOADER WRITTEN TO 0xA0000000`, `HANDOFF TO THE LOADER` |
| Loader | title `LOAD DIAG T1T`, rows 9 to 24 | CAR words, clock and reset state, six sources, `PLLP_BASE`, then per controller `CNFG`/`STATUS`/`INT`, `DIV`/`LOAD`, `TIMING_0`, `TIMING_1`, the comparison, `T1T COMPLETE CODE 0000`, `REGISTER READS 25` |
| End | row 26 and the countdown | `BOOT STAYS 0`, `RESET IN 15 S`, then hekate |

No value is a pass or fail. T1-R measured `CLK_SOURCE_I2C1` `00000003`, `ENB_L` `9802D131`, `PLLP_BASE` `48115408` and
`CONFIG_LOAD` `00000004`. A different value is a result to report, not an error to fix.

Reading the screen:

- The `DIV` line prints the raw word and then the upper 16 bits with no separator, so `04D102CA04D1` is the word `04D102CA`
  followed by `04D1`. Read the first 8 digits as the word.
- The source fields are not decoded per bus. Take bits 31:30 from the raw word.
- `NOT READ: CLOCK OFF` or `NOT READ: IN RESET` for a controller means none of its registers was touched. `COMPARISON NOT
  POSSIBLE` follows.
- `T1T STOPPED CODE 0701` means the allowlist refused a read. That is a loader bug. Photograph it and stop.

## 6. Time

| Stage | Estimate |
| --- | --- |
| Selection to the first U-Boot line | not measured for T1T. Deadline 20 s |
| U-Boot lines and `bootm go` | seconds |
| Loader report | 25 register reads, no waits. Under a second |
| Countdown | 15 s, fixed in `exit.c` |
| Reset to hekate's menu | not measured separately |

The older D0 diagnostic took about 21 s in total (reported). T1-T has no transfer and no wait, so it should not exceed that by
more than a few seconds. This is an estimate. The 50 s total and the stage deadlines are provisional human deadlines.

## 7. Abort and return

Hold the power button until the console is off, and report the last line and how long it stood there, if:

- 50 s have passed since the start and hekate has not returned;
- a stage passes the deadlines of `ERISTA_T1_RUN.md` section 5;
- the screen shows an exception, `T1T STOPPED CODE`, `NO RESET`, `PSCI SYSTEM_RESET RETURNED`,
  `EXIT CALLED OUTSIDE EL1`, a black screen or garbage;
- the console feels hot.

A screen that stops changing is a stall. T1-T has no timeout inside a register read. If a read hangs, the loader cannot end it
and only the button does. The clock and reset gate is there to avoid that; it is not shown to prevent it.

Return to hekate: after the report the loader waits 15 s and calls PSCI `SYSTEM_RESET`. T1-R returned to hekate by itself this
way on 2026-11-26. For T1-T it is unverified until it happens. An exception ends the same way, since the exception screen
also goes to the 15 s wait. Nothing was written, so nothing needs restoring.

What the checks do not cover:

- The inspection finds device addresses in the flat image and in `movz`/`movk` immediates, but it skips the `0x7000` page for
  immediates, because the identity registers `70000804` and `7000f9d8` live there. A scan of this image found only those two.
  A later change that built an I2C address from immediates would not be caught by the inspection.
- Read side effects of the timing registers and of every I2C5 register are undocumented in the sources read. See
  `ERISTA_T1T_DESIGN.md`.

## 8. After the run

One run. Do not retry. After an abnormal end, power off, wait 10 s and follow the Nyx recovery check in `ERISTA_T1_RUN.md`
section 6.

Report the video, the selection-to-menu time, the Nyx status-bar value and a photograph of the loader screen sharp enough to
read every digit. Transcribe the words from the photograph, not from this sheet.
