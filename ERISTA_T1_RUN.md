# T1 v2: the one hardware run

Approved 2026-11-25 for T1 v2 only: the Mac artifacts of commit `86346c224`, no code change, one run, no retry after any
abnormal end. T2 to T4 and D1 hardware boot stay on hold. `boot` stays 0.

Success means the I2C1 and TMP451 transfers completed and the ID read 0x55. It does not mean a real temperature was read, that
the sensor settings are right, or that any thermal protection works. The 45 s total deadline is a point for the person to
act, not a thermal limit.

## 1. Copy (Mac)

The package build is of `86346c224`; later commits are documentation, which this shows:

```
git diff --stat 86346c224 HEAD -- boot tools kits bazel
```

Empty output is expected. Then, with the experiment card backed up and mounted (replace `CARD`):

```
export COPYFILE_DISABLE=1
PKG="$PWD/bazel-bin/boot/diag_t1/package"
SD="/Volumes/CARD"
mkdir -p "$SD/switchroot/ios-diag-t1"
cp "$PKG"/switchroot/ios-diag-t1/bl31.bin "$PKG"/switchroot/ios-diag-t1/bl33.bin "$PKG"/switchroot/ios-diag-t1/script.txt "$SD/switchroot/ios-diag-t1/"
cp "$PKG"/bootloader/ini/ios-diag-t1.ini "$SD/bootloader/ini/"
sync
ls -la "$SD/switchroot/ios-diag-t1" "$SD/bootloader/ini"
```

The `ls` shows the four new files and the existing entries unchanged, with no `._` files.

## 2. Check the hashes (Mac)

```
cd "$SD" && shasum -a 256 -c <<'SUMS'
dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4  switchroot/ios-diag-t1/bl31.bin
23ac56e88271afb49d09ed5684a8a223d5fb63f280a6d93c99f2b1d5cd5ac87f  switchroot/ios-diag-t1/bl33.bin
f762d2a30ce57ee1a4f38a96f7e2c2bc8e347669814dedb07d615dec48da01ef  switchroot/ios-diag-t1/script.txt
c32c631a13ee5f02b3b7b76aa45b23d159f02c22c4ebb4017f4b0ae245576906  bootloader/ini/ios-diag-t1.ini
SUMS
```

All four must print `OK`. Eject the card (`diskutil eject "$SD"`), insert it again and run the check a second time, so the
read comes from the card and not the cache. Any `FAILED` or missing file: do not run.

## 3. Before starting

- [ ] Battery 100 %, charger out, hard surface, vents clear, no case, console cool to the touch.
- [ ] hekate 6.5.5, Auto Boot off, started from Nyx "More configs".
- [ ] Phone recording from before `More configs` until hekate returns, with a clock or timer in frame if possible (this is the
      elapsed-time measurement: no on-screen timer in v2).
- [ ] You stay with the console, a hand near the power button.
- [ ] Say the start time as you select `iOS Diag T1`. Total deadline: 45 s from then.

## 4. Expected screen

U-Boot lines as D0 (`BOARD OK`, `FRAMEBUFFER OK`, `DTB COPIED AND FRAMEBUFFER RESERVED`, `LOADER WRITTEN TO 0xA0000000`,
`HANDOFF TO THE LOADER`), then the screen clears and shows the T1 title, then:

| Row | Expected |
| --- | --- |
| Board | `EL1 STATE PASS`, Erista, `T210B01=0`, `SKU=0`, `BOOT=0` |
| CAR | I2C1 clock on, out of reset; PWM clock state shown, no expectation |
| I2C1 | `DIV 00050001`, `STATUS` BUSY (0x100) clear; other words as hekate left them |
| Pads | `SCL 00000040 SDA 00000040` (reported, not judged) |
| TMP451 | `ID(FE) 55`; `CONFIG(03)` C0 expected on this path (80 or 00 possible); `RATE(04)` 02 expected (08 is power-on) |
| Raw | two bytes labelled `NOT A TEMPERATURE` |
| Result | `T1 COMPLETE CODE 0000`, `TRANSFERS 10 OF 10` |
| End | `RESET IN 15 S`, then hekate's menu on its own |

`T1 STOPPED CODE` lines: 0501 clock off, 0502 in reset, 0503 BUSY before start, 0504 divisor not 0x00050001, 0505
`CONFIG_LOAD` stuck, 0506 BUSY stuck, 0507 no ACK, 0508 ID not 0x55, 0509 BUSY after the last transfer, 050A allowlist
refusal, 050B not an Erista, 050C counter. A stop code is a result: the loader still draws it, waits 15 s and resets.
Photograph or film it and do nothing else.

## 5. Abort

Hold the power button until the console is off, and report the last line and the time it stood there, if:

- 45 s have passed since the start and hekate has not returned;
- any stage passes its deadline (20 s to the first U-Boot line, 10 s between lines, 10 s from `HANDOFF` to the loader's first
  line, 20 s to the report, 25 s from the countdown to hekate);
- `NO RESET`, `PSCI SYSTEM_RESET RETURNED`, `NO EL CHANGE`, `EXIT CALLED OUTSIDE EL1`, an exception screen, a black screen
  or garbage;
- the console feels hot.

The screen stuck with no text change is a stall: a hung device read or a `wfe` that is not woken cannot be ended by the
loader. Only the button does.

## 6. After the run

One run only. No second attempt after any stop, abort or odd screen, until the reviewer has read it.

After an abnormal end (any stop code, abort, or button hold) power off completely, wait 10 s, then:

1. Power on to hekate and open Nyx. Its start sets the sensor running; its status bar updates every 5 s.
2. Watch the status bar for 30 s: the clock moves and the SoC temperature shows a plausible number. The same number for a
   long time on an idle console is not a fault. Do not heat the console. `00.0`, a stopped clock or an abnormal screen is.
3. The fan starts at 42 C, so silence at idle is normal. Console hot with a silent fan: power off and report.

After a clean run (hekate returned on its own) steps 1 and 2 are a quick look.

Report: the photograph or video, the time from selection to hekate's menu, the status-bar value and Nyx uptime.
