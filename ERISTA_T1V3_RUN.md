# T1 v3: run sheet

T1 v3 hardware boot is on hold. This sheet is for the run once it is approved. One run, no automatic retry, `boot` stays 0.

T1 v3 changes one register: the I2C1 `CLK_DIVISOR`, from `00050001` to `00FF0001`, then reads the TMP451 manufacturer ID
(`0x55`) at address `0x4C`. It writes eleven words, all to I2C1, in a fixed order. It never writes `TIMING_0`, `TIMING_1`, I2C5
or the CAR. The 50 s total is a point for the person to act, not a thermal limit.

Approved pins (2026-11-26, from `ERISTA_T1T_RESULT.md`): `TIMING_0` `00000204`, `TIMING_1` `04070404`. The run goes on only if
I2C1 reads exactly these words. They are one observation and are not shown to be reset values.

## 1. Build and verify (Mac)

```
bazel test //boot/diag/loader/test:t1v3_test //boot/diag/loader/test:t1v3_measured_test //tools/package/test:diag_t1v3_test //tools/package/test:diag_t1t_test //tools/package/test:diag_t1t_measured_test //tools/package/test:diag_loader_test //tools/package/test:diag_mmio_audit_test --config=ios17
bazel build //boot:diag_t1v3 --config=ios17
bazel aquery //boot:diag_t1v3 --config=ios17 --output=text | grep -c -- '--t1v3'
PKG="$PWD/bazel-bin/boot/diag_t1v3/package"
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256)
PYTHONPATH=tools/package python3 tools/package/diag_mmio_audit.py \
  --disassembly bazel-bin/boot/diag_t1v3/loader.dis \
  --uboot-image bazel-bin/boot/diag_t1v3/loader.uboot.bin --t1v3
```

The `--t1v3` count must be 1 or more. The audit must end with `AUDIT PASSED` and `6. Flat image: register address words match the
T1 v3 list`. Its Linux output is `boot/diag/erista-t1v3-mmio-audit.txt`: only the two identity registers are formed in code, and
`switch_t1v3_read32` and `switch_t1v3_write32` are called only by `switch_t1v3_access`.

The Mac build of commit `f8cc80dcc` gave these (reported by the user, 2026-11-26). They are the values to check on the card.

| File | SHA-256 |
| --- | --- |
| `switchroot/ios-diag-t1v3/bl31.bin` | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` |
| `switchroot/ios-diag-t1v3/bl33.bin` | `34f08be629e6757272735b97d20a71a6c4c41311dbc0c84a475b481bdbd89896` |
| `switchroot/ios-diag-t1v3/script.txt` | `f762d2a30ce57ee1a4f38a96f7e2c2bc8e347669814dedb07d615dec48da01ef` |
| `bootloader/ini/ios-diag-t1v3.ini` | `64cc262c478256234adbc9780a173eb1e00933c702f7f91046b7972eb8ba821b` |

`bl31.bin` and `script.txt` equal the other diagnostics'. Against the Mac build before the pins, only `bl33.bin` changed. On the
Mac the 7 Bazel tests passed, `aquery` showed `--t1v3` once, the audit printed `AUDIT PASSED`, and the 24 files of the six other
images matched the earlier Mac hashes. The loader's flat image hash on the Linux host is `de7ea6af...16ea`
(`tools/package/diag_images.py`, clang 18.1.3); the Bazel clang may give another value, so it is not a card check.

The six other images must not change. With the same compiler on both sides:

```
git worktree add ../before 5f474ed0d
(cd ../before && bazel build //boot:diag_loader //boot:diag_loader_r //boot:diag_loader_sd //boot:diag_t1 //boot:diag_t1r //boot:diag_t1t --config=ios17)
bazel build //boot:diag_loader //boot:diag_loader_r //boot:diag_loader_sd //boot:diag_t1 //boot:diag_t1r //boot:diag_t1t --config=ios17
shasum -a 256 ../before/bazel-bin/boot/diag_*/package/switchroot/*/* bazel-bin/boot/diag_*/package/switchroot/*/*
```

On Linux the six hashes (`boot/diag/erista-diag-image-hashes.txt`) are the same before and after the pin change.

## 2. Copy and check (Mac)

```
export COPYFILE_DISABLE=1
SD="/Volumes/CARD"
mkdir -p "$SD/switchroot/ios-diag-t1v3"
cp "$PKG"/switchroot/ios-diag-t1v3/bl31.bin "$PKG"/switchroot/ios-diag-t1v3/bl33.bin "$PKG"/switchroot/ios-diag-t1v3/script.txt "$SD/switchroot/ios-diag-t1v3/"
cp "$PKG"/bootloader/ini/ios-diag-t1v3.ini "$SD/bootloader/ini/"
sync
ls -la "$SD/switchroot/ios-diag-t1v3" "$SD/bootloader/ini"
```

```
cd "$SD" && shasum -a 256 -c <<'SUMS'
dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4  switchroot/ios-diag-t1v3/bl31.bin
34f08be629e6757272735b97d20a71a6c4c41311dbc0c84a475b481bdbd89896  switchroot/ios-diag-t1v3/bl33.bin
f762d2a30ce57ee1a4f38a96f7e2c2bc8e347669814dedb07d615dec48da01ef  switchroot/ios-diag-t1v3/script.txt
64cc262c478256234adbc9780a173eb1e00933c702f7f91046b7972eb8ba821b  bootloader/ini/ios-diag-t1v3.ini
SUMS
find "$SD" -name '._*' -path '*ios-diag-t1v3*'
```

All four print `OK` and `find` prints nothing. Eject (`diskutil eject "$SD"`), insert again and run the check a second time. Any
`FAILED`, missing file or `._` file: do not run. Older entries stay on the card.

## 3. Before starting

Same as `ERISTA_T1_RUN.md` section 3 with the entry `iOS Diag T1V3` and a 50 s deadline from selecting it. Start from a cold
state: power the console off, wait 10 s, boot hekate, and note the Nyx temperature before selecting the entry. Film the screen
from the first U-Boot line. Launch from Nyx "More configs", as T1-T was.

## 4. What it writes

| # | Register (I2C1, `7000c000` +) | Value | Purpose |
| --- | --- | --- | --- |
| 1 | `CLK_DIVISOR` `+6c` | `00FF0001` | the one change |
| 2 | `CONFIG_LOAD` `+94` | `01` | applies it |
| 3 | `CMD_ADDR0` | `98` | TMP451 write address |
| 4 | `CMD_DATA1` | `FE` | pointer to the manufacturer ID |
| 5 | `CNFG` | `2800` | write, one byte, no go |
| 6 | `CONFIG_LOAD` | `25` | as hekate |
| 7 | `CNFG` | `2A00` | go |
| 8 | `CMD_ADDR0` | `99` | read address |
| 9 | `CNFG` | `2840` | read, no go |
| 10 | `CONFIG_LOAD` | `25` | as hekate |
| 11 | `CNFG` | `2A40` | go |

Nothing is written to `TIMING_0`, `TIMING_1`, I2C5 or the CAR. The allowlist refuses every other register (stop `0610`), and no
write is made once a stop code exists (`put()` checks it first). Nothing is restored after a stop; the divisor stays at
`00FF0001` until a reset.

## 5. Gates before write 1

Any miss stops with nothing written. The order is the order of the code.

| Check | Stop |
| --- | --- |
| Not an Erista | entry check in `diag.c`: the screen says `NOT AN ERISTA. NO T1V3 REGISTER TOUCHED`, the run never starts (`0611` is only the report name) |
| Counter advances | `0612` |
| I2C1 clock on, not in reset | `0601`, `0602` |
| `CLK_SOURCE_I2C1` is `00000003` | `0603` |
| `PLLP_BASE` fields M 8, N 85, P 1 | `061B` |
| `STATUS` BUSY clear | `0604` |
| `INT_STATUS` is 0 | `0613` |
| `CLK_DIVISOR` is `00050001` | `0605` |
| `CONFIG_LOAD` bit 0 clear, word is `00000004` | `0616`, `0615` |
| `TIMING_0` and `TIMING_1` not 0, not all ones | `0606`, `0608` |
| Both equal their pins, bit for bit | `0607` |
| Three samples, 100 us apart, idle and equal to the first | `0609` |

I2C5's BUSY and pending flags are not read. They share no register with I2C1; the only shared thing is the PLL, which the run
does not touch.

## 6. Expected screen

| Point | Where | What is shown |
| --- | --- | --- |
| U-Boot | the D0 lines | `BOARD OK`, `FRAMEBUFFER OK`, `DTB COPIED AND FRAMEBUFFER RESERVED`, `LOADER WRITTEN TO 0xA0000000`, `HANDOFF TO THE LOADER` |
| Loader | rows 9 to 20 | CAR words, `PLLP_BASE` `48115408` with M 8 N 85 P 1, before words, `TIMING_0 00000204`, `TIMING_1 04070404`, after words, the `CONFIG_LOAD` words after each `0x25`, start and clear times, the read words, ID, stop line, write and transfer counts |
| End | footer and countdown | `BOOT STAYS 0`, `RESET IN 15 S`, then hekate |

A full run reads `T1V3 COMPLETE CODE 0000`, `WRITES 11`, two transfers, the ID `55`, `DIV 00FF0001` after the apply. The before
words should match T1-T: `CNFG 00002842`, `STATUS 0`, `INT 0`, `DIV 00050001`, `LOAD 00000004`.

Times are reference values, not an SCL measurement.

## 7. Reading the result

| Screen | Meaning |
| --- | --- |
| `COMPLETE`, ID `55` | the TMP451 ACKed at the slower rate and returned its ID. Under the model SCL is about 49.8 kHz; the module clock is a model |
| `0601` to `0609`, `0612` to `0616`, `061B` before any write | a gate stopped. Report the code and the words shown |
| `060D` NOACK | the device did not answer. It does not show a timing fault: if TIMING_1 counts module cycles, setup and hold at the slower divisor are about 21 times shorter in time than under hekate's 4.8 MHz source (unverified unit) |
| `060B` | divisor readback differs. The divisor is not restored |
| `060A`, `0617` | `CONFIG_LOAD` anomaly. No transfer started from it |
| `060C`, `0618` | BUSY never cleared or never seen. No retry |
| `060E` | wrong ID. The data path answered with something else |
| `061A` | a pin is 0. A build mistake. It writes nothing |
| `0619`, `060F`, `0614` | stale `CMD_DATA1`, BUSY after the apply, `INT_STATUS` changed. No retry |

`CONFIG_LOAD` after the apply is accepted as `0` or `4`. A `0` says only that a write without bit 2 clears it; `4` says it was
not cleared. Neither says whether hardware or the write did it. No safety conclusion rests on it.

## 8. Time and abort

| Stage | Estimate |
| --- | --- |
| Selection to the first U-Boot line | not measured for T1V3. Deadline 20 s |
| Loader work | three idle samples, two transfers of a few bytes at about 50 kHz, so under a second |
| Countdown | 15 s, fixed in `exit.c` |
| Reset to hekate's menu | not measured separately |

This is an estimate. The 50 s total and the stage deadlines are provisional human deadlines.

Hold the power button until the console is off, and report the last line and how long it stood there, if:

- 50 s have passed since the start and hekate has not returned;
- a stage passes the deadlines of `ERISTA_T1_RUN.md` section 5;
- the screen shows an exception, `NO RESET`, `PSCI SYSTEM_RESET RETURNED`, `EXIT CALLED OUTSIDE EL1`, a black screen or garbage;
- the console feels hot.

After a stop the loader waits 15 s and calls PSCI `SYSTEM_RESET`, as T1-T did (hekate returned by itself, Nyx 34.6 C). The reset
returns the SoC to its boot path. Whether it also resets I2C1's registers is not observed; hekate's `i2c_init` writes the divisor
`00050001` at its start (source reading, not observed). If hekate's menu or Nyx behaves oddly after a run, power off and wait
10 s before trying again.

A loop ends by count (1,000,000 iterations), so a stopped timer ends a wait with a stop code. A register read that hangs the bus
cannot be ended by software; only the button does.

## 9. Review of the open items

Done offline on 2026-11-26. No write path was changed.

1. **`CONFIG_LOAD` bits 2 and 5.** I2C1 reads bit 2 set, I2C5 reads it clear, and bit 5 reads 0 on both. The gate and the
   transfers do not depend on why. The existing implementation requires `0000 0004` before write 1, accepts `0` or `4` after the
   `0x01`, and requires exactly `4` after each `0x25` before the go. A word outside that set stops with `0617` before any
   transfer. A bit 5 that read 1 would stop the same way. A 0/4 readback after `0x01` does not separate "a write clears bit 2"
   from "only hardware clears it": under both, `0x25` ends at `4`. It is a record, not a proof.
2. **`TIMING_1` unit.** Unknown. If it counts module cycles, T1 v3's setup and hold are far shorter in time than hekate's. If it
   counts divided cycles, the divisor scales it. T1 v3 writes neither word, so this affects how to read a NOACK, not what the
   run does.
3. **I2C5 BUSY.** I2C5 read BUSY set with `INT_STATUS` `000000C2` in T1-T. It has no register in common with I2C1. The T1 v3
   gate reads I2C1 only. The CAR source and PLL are shared and are checked once before write 1, not during the run. No CAR
   re-read after the divisor write exists; a read-only re-read of `CLK_SOURCE_I2C1` could be added if wanted.
4. **After a stop.** The divisor stays `00FF0001` until a reset. The 15 s wait and `SYSTEM_RESET` follow every stop. hekate's
   return is observed for T1-T (and T1-R); for T1 v3 it is unverified.
5. **Gates and writes after a stop.** Every gate in section 5 runs before write 1. A stop can appear only after a load poll
   (after writes 2, 6, 10) or a go (writes 7, 11); the host test `EveryStopAfterTheFirstWriteEndsTheWritesAtItsOwnPointWithListedWordsOnly`
   lists the exact write counts, 2, 6, 7, 10 and 11.
6. **One finding.** The name of stop `0607` reads "I2C1 TIMING_0 IS NOT 00000204" but the code also uses `0607` for a `TIMING_1`
   mismatch. The screen shows both words, so the cause is readable. The text is not changed here.

## 10. Final review before the run

Done offline on 2026-11-26 against `t1v3.c`, `t1v3_regs.c` and the host tests. No device code was changed. Nothing here was run on
hardware.

| Item | Result |
| --- | --- |
| 11 writes | Address, value and order are the table in section 4. Host test `NormalRunWritesTheElevenReviewedWordsInOrder` and the new `T1v3Pins` tests compare the exact list. The allowlist holds five I2C1 registers and these values only |
| Gates and stop codes | Section 5 is the code order: counter, the four CAR reads and their four checks, the seven I2C1 reads and their checks, three idle samples. Each stop is tested with zero writes |
| `CONFIG_LOAD` after the divisor change | Bit 0 must clear within 1 ms (`060A`). Then the word must be `0` or `4`, else `0617` before any transfer. Before each go it must be exactly `4`. The divisor readback must be `00FF0001` (`060B`), `STATUS` BUSY clear (`060F`), `INT_STATUS` unchanged (`0614`) |
| BUSY, NOACK, timeout | BUSY must rise within 1 ms of the go (`0618`) and clear within 100 ms (`060C`). NOACK bits are read after BUSY clears (`060D`). A NOACK on the pointer write ends the run before the read. Every poll also ends after 1,000,000 iterations |
| No write after a stop | `put()` returns before the allowlist and the device once a stop code exists. A source test holds that order. Stops can only be raised after a load poll or a go, and the scenario table lists the write counts 2, 6, 7, 10 and 11 |
| Recovery | section 8. No restore, no bus clear, no controller reset |
| Temperature and time | the loader reads no temperature: it reads the TMP451 manufacturer ID, not a temperature register. The limit is the person's: Nyx value before and after, a 50 s deadline, power button on heat. The run itself is under a second plus the 15 s countdown (estimate) |
| Card copy and check | section 2, with the Mac hashes above |

Findings, none changed:

- **The reset path and I2C1 are not known to be independent.** After a stop the divisor stays at `00FF0001` and the loader calls PSCI
  `SYSTEM_RESET`. Whether BL31's reset path or anything that runs before hekate's `i2c_init` talks to I2C1 is not observed. If it
  does, it runs at the slower rate. No evidence either way. Treat a hang after the countdown as a result and report it.
- **A NOACK is ambiguous** (section 7): the `TIMING_1` unit is unknown, so no conclusion about timing follows.
- **No on-device temperature stop exists.** The deadlines and the console's heat are the only brakes, and both are human.
- **Other devices on I2C1** (charger, fuel gauge) see the slower bus only after write 2. No other master is known in the loader
  window. That is a reading of code that was not run.

## 11. After the run

One run. Do not retry. After an abnormal end, power off, wait 10 s and follow the Nyx recovery check in `ERISTA_T1_RUN.md`
section 6.

Report the video, the selection-to-menu time, the Nyx value before and after, and a photograph of the loader screen sharp enough
to read every digit. Transcribe the words from the photograph, not from this sheet.
