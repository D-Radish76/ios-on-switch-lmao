# T2: run sheet

T2 hardware boot is on hold and the TMP451 write is not approved. This sheet is for the run once the owner approves it in writing. One run, no retry,
`boot` stays 0, the feature mask stays 0. T3, T4, the full D1 and every XNU run stay on HOLD.

T2 does what T1 v3 did (I2C1 divisor `00FF0001`, then reads at address `0x4C`) and adds one sensor write: the one-shot, pointer `0x0F`, value `0x00`.
It writes no CAR register, no I2C5, no timing register, no other sensor register. The 50 s total is a point for the person to act, not a thermal limit.
Design and review: `ERISTA_T2_DESIGN.md`, `ERISTA_T2_REVIEW.md`.

## 1. Build and verify (Mac)

```
bazel test --config=ios17 //boot/diag/loader/test:t2_bus_test //tools/package/test:diag_t2_test //tools/package/test:diag_t2_audit_test \
  //tools/package/test:diag_mmio_audit_test //tools/package/test:diag_loader_test
bazel build --config=ios17 //boot:diag_t2
PKG="$PWD/bazel-bin/boot/diag_t2/package"
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256)
PYTHONPATH=tools/package python3 tools/package/diag_mmio_audit.py \
  --disassembly bazel-bin/boot/diag_t2/loader.dis \
  --uboot-image bazel-bin/boot/diag_t2/loader.uboot.bin --t2 --t2-sources boot/diag/loader
```

The audit must end with `AUDIT PASSED (T2)` and show `6. Flat image: register address words match the T2 list`. Its Linux output is
`boot/diag/erista-t2-mmio-audit.txt`. No Mac hash of the T2 package has been reported yet, so none is written here: the Mac's four values (`bl31.bin`,
`bl33.bin`, `script.txt`, `ios-diag-t2.ini`) go into the check in section 2 when it reports them. `bl31.bin` and `script.txt` should equal the other
diagnostics'; only `bl33.bin` and the ini should differ. The Linux flat hash is `7f5a91a4...f124` (`boot/diag/erista-diag-image-hashes.txt`); the Mac's
clang may give another, so it is not a card check.

The nine earlier images must not change: `diag_images.py` prints their nine Linux hashes, and the Mac's `loader.uboot.bin` of `diag_state`,
`diag_d1min` and `diag_loader_r` stay one file.

## 2. Copy and check (Mac)

```
export COPYFILE_DISABLE=1
SD="/Volumes/CARD"
mkdir -p "$SD/switchroot/ios-diag-t2"
cp "$PKG"/switchroot/ios-diag-t2/bl31.bin "$PKG"/switchroot/ios-diag-t2/bl33.bin "$PKG"/switchroot/ios-diag-t2/script.txt "$SD/switchroot/ios-diag-t2/"
cp "$PKG"/bootloader/ini/ios-diag-t2.ini "$SD/bootloader/ini/"
sync
(cd "$SD" && shasum -a 256 switchroot/ios-diag-t2/* bootloader/ini/ios-diag-t2.ini; find . -name '._*' -path '*ios-diag-t2*')
```

Compare the four hashes with the Mac build's, eject, insert again and compare a second time. A mismatch, a missing file or a `._` file: do not run.
Older entries stay on the card. The entry is `iOS Diag T2`; launch it from Nyx "More configs", as T1-T and T1 v3 were.

## 3. Before starting

| Check | How |
| --- | --- |
| Written approval for the one sensor write | the owner, separate from every earlier approval |
| `boot` 0 and the feature mask 0 | the Erista row; no `iOS` entry is bootable on this card |
| hekate 6.5.5, the entry list photographed | as the earlier runs |
| Cold start | power off, wait 10 s, boot hekate |
| Nyx temperature written down **before selecting the entry** | the figure goes in the record; D0-S v2's start was lost. Start only below the provisional 40 C |
| Battery charged, console on a table, Joy-Con off | as D1-min |
| Someone at the console who has read sections 6 to 8 | POWER is the only stop that does not depend on the loader |
| Video on from the first U-Boot line | the screen is the record |

## 4. What it writes

| Step | Writes | Where |
| --- | --- | --- |
| Prelude | 2: `CLK_DIVISOR` `00FF0001`, `CONFIG_LOAD` `01` | I2C1 `+6c`, `+8c` |
| Each sensor read | 9: `CMD_ADDR0` `98`, `CMD_DATA1` pointer, `CNFG` `2800`, `CONFIG_LOAD` `25`, `CNFG` `2A00`, then `CMD_ADDR0` `99`, `CNFG` `2840`, `CONFIG_LOAD` `25`, `CNFG` `2A40` | I2C1 |
| The one-shot | 5: `CMD_ADDR0` `98`, `CMD_DATA1` `0000000F`, `CNFG` `2802`, `CONFIG_LOAD` `25`, `CNFG` `2A02` | I2C1; on the TMP451 it is one two-byte write: pointer `0F`, value `00` |

The sequence reads 14 registers before the polling (ID, configuration, two offsets, status, four old values), then status polls (at least two:
about 2 ms and about 31 ms), the configuration again and five new values. With `n` polls that is `142 + 9n` writes, so `160` or more; the design
estimated 150 to 190. The screen's `WRITES` is the figure. The host model in `t2_bus_test` gives the same count; no run has. Nothing is written to the CAR, `TIMING_0`,
`TIMING_1` or I2C5, and the allowlist refuses every other word (`ACCESS REFUSED BY THE ALLOWLIST. LOADER BUG`, `0710` or `0730`). Once a stop code
exists nothing more is written. Nothing is restored after a stop.

## 5. Order and gates

The controller gates are T1 v3's. Rows 1 to 7 run before write 1: a miss there stops with nothing written. Row 8 contains write 1 and its
checks come after it: a miss there leaves the divisor at `00FF0001`, not restored, and the loader makes no further transfer.

| Order | Check | Stop |
| --- | --- | --- |
| 1 | not an Erista | the entry check in `diag.c`: row 9 says `NOT AN ERISTA. NO T2 REGISTER TOUCHED` and the run never starts (`0711` is the report name only) |
| 2 | counter advances | `0712` |
| 3 | I2C1 clock on, not in reset | `0701`, `0702` |
| 4 | `CLK_SOURCE_I2C1` `00000003`; `PLLP_BASE` M 8, N 85, P 1 | `0703`, `071B` |
| 5 | `STATUS` BUSY clear; `INT_STATUS` 0; divisor `00050001`; `CONFIG_LOAD` bit 0 clear and word `00000004` | `0704`, `0713`, `0705`, `0716`, `0715` |
| 6 | `TIMING_0` `00000204`, `TIMING_1` `04070404`, not 0, not all ones | `0706` to `0708` |
| 7 | three idle samples 100 us apart | `0709` |
| 8 | divisor write, `CONFIG_LOAD` `01`, bit 0 clears in 1 ms, readback `00FF0001`, `CONFIG_LOAD` `0` or `4`, BUSY clear, `INT_STATUS` unchanged | `070A`, `070B`, `0717`, `070F`, `0714` |
| 9 | sensor ID `55` | `0721`: no sensor write |
| 10 | configuration `C0` | `0722`: no sensor write |
| 11 | status BUSY clear | `0723`: no sensor write |
| 12 | the one-shot | `0724` if it fails; no retry |
| 13 | poll until BUSY clear, not accepted before 31 ms, 100 ms bound | `0725` |
| 14 | configuration again, then the new values | `0720` for a failed read |

## 6. Expected screen

| Point | Where | What is shown |
| --- | --- | --- |
| U-Boot | the D0 lines | `BOARD OK`, `FRAMEBUFFER OK`, `DTB COPIED AND FRAMEBUFFER RESERVED`, `LOADER WRITTEN TO 0xA0000000`, `HANDOFF TO THE LOADER` |
| Loader | rows 9 and down, in this order | CAR words, `BEFORE CNFG ...`, `TIMING_0 ... TIMING_1 ...`, `AFTER LOAD ... DIV WRITTEN 00FF0001, NOT RESTORED`, `TMP451 0X4C ID(FE) ...`, `BEFORE THE ONE-SHOT REMOTE ... LOCAL ...`, `ONE-SHOT WRITTEN. BUSY ...`, `AFTER REMOTE ... CONFIG ...`, the temperature line, then row 21 `T2 COMPLETE CODE 0000. COMPLETE`, row 22 `WRITES ... TRANSFERS ... NO RETRY. NO CAR WRITE. ONE SENSOR WRITE AT MOST` |
| End | footer | `BOOT STAYS 0`, `RESET IN 15 S`, then hekate |

A line is drawn red when it is a stop, `NOT IN SHUTDOWN` or inconsistent, or the temperature is not a reading.

A full run: ID `55`, configuration `C0`, status `00` before, `BUSY SEEN AT` a figure (or `NOT SEEN`), `DONE AT` 31000 us or more and below 100000,
configuration `C0` after with `BACK IN SHUTDOWN`. Before words equal T1 v3's: `CNFG 00002842`, `STATUS 0`, `INT 0`, `DIV 00050001`, `LOAD 00000004`.

## 7. Reading the result

**BUSY and the timeout.** The first status read is at about 2 ms. No completion is accepted before 31 ms, the TI minimum; reads follow at about 31 ms,
34 ms, then every 5 ms. BUSY clear at or after 31 ms is done (`DONE AT`). BUSY still set at 100 ms is `0725`, `CONVERSION NOT DONE IN 100 MS. NO RETRY`.
The 100 ms is a design bound, about three times typical, not a TI figure. `BUSY NOT SEEN` is recorded and is not a failure: whether BUSY is set during a
one-shot is unproven. The times are reference values of a polling loop.

**Temperature decode.** Each value is a high byte and a low byte. Sixteenths = high x 16 + (low >> 4), degrees = sixteenths / 16. Example: high `22`
and low `80` are 34 + 8/16 = 34.5 C (high `0x22` = 34). The range is 0 to 127 C. The screen decodes the new pair; the old pair is the sensor's earlier
conversion and is shown raw.

| Screen | Meaning |
| --- | --- |
| `REMOTE nn.nnnn C, LOCAL nn.nnnn C. ONE READING...` | two readings after the one-shot. Not a limit and not a safety criterion |
| `REMOTE DIODE OPEN OR READING AT THE RANGE EDGE` | OPEN set, or the remote high byte is 0 or 127: not a reading |
| `THE TWO REMOTE HIGH READS DIFFER` | the pair is not from one conversion. No temperature shown |
| the local value | the PCB sensor; the remote value is the SoC diode. Nyx shows its own readings; compare them, do not assume equality |

| Code | Meaning |
| --- | --- |
| `0701` to `0709`, `0711` to `0713`, `0715`, `0716`, `071B` | a gate in rows 1 to 7 stopped before write 1. Nothing was written. Report the code and the words shown |
| `070A`, `070B`, `070F`, `0714`, `0717` | a check of row 8, after the divisor write. The divisor is `00FF0001` and stays so until a reset; no sensor transfer was made |
| `0734`, `0735` | `CONFIG_LOAD` anomaly on a transfer. No go was written from it |
| `0721`, `0722`, `0723` | the sensor is not what the design assumes. No sensor write was made |
| `0720`, `0733` | a read failed or no ACK from `0x4C`. No retry |
| `0724` | the one-shot failed. Whether the part started a conversion is unknown |
| `0725` | BUSY set after 100 ms. No later reads, no restore |
| `0731`, `0732`, `0736` | BUSY never seen after a go, BUSY after 100 ms, counter stalled |
| `0710`, `0730` | the allowlist refused an access. A loader bug: report it |

## 8. Time and abort

About 40 s from the first U-Boot line to hekate (an estimate from D0 and D1-min: U-Boot's steps, a bus time under 100 ms with the conversion wait, then
`RESET IN 15 S` and PSCI `SYSTEM_RESET`). A 50 s total from selecting the entry is provisional. Hold the power button until the console is off, and
report the last line and how long it stood there, if:

- 50 s have passed since the start and hekate has not returned;
- the screen shows an exception, `NO RESET`, `PSCI SYSTEM_RESET RETURNED`, `EXIT CALLED OUTSIDE EL1`, a black screen or garbage;
- the console feels hot or Nyx shows 50 C or more (provisional, not an official limit).

After a stop or a finish the loader waits 15 s and calls PSCI `SYSTEM_RESET`, which returned to hekate in the D0, D0-R, T1-T, D0-S v2 and D1-min runs.
That is the automatic path; the held power button is the manual one, and Nyx is where the temperature is read. A loop ends by count (1,000,000
iterations); a register read that hangs the bus cannot be ended by software.

## 9. Does the sensor stay in shutdown?

T2 writes no setting, so shutdown should remain. The screen answers it twice: the configuration read before is `C0` (required, or `0722` stops
before the write) and the one read after the conversion shows `BACK IN SHUTDOWN` for `C0`, or `SHUTDOWN BIT CLEAR`, drawn red, which is a finding and
is not corrected. Nyx cannot answer it afterwards: hekate's `tmp451_init` rewrites the configuration at its start, so a normal Nyx temperature proves
nothing about the state T2 left. Only the screen line counts.

## 10. Retry is forbidden

No second run in the same sitting or the next, whatever the result, without a new written approval. A retry is specifically refused after:

- any stop code after the one-shot write (`0724`, `0725`, `0720`, `0731`, `0732`, `0733`, `0736`), because the sensor's state is unknown;
- `SHUTDOWN BIT CLEAR` or `THE TWO REMOTE HIGH READS DIFFER`;
- a hang, a panic screen, `PSCI SYSTEM_RESET RETURNED`, or hekate or Nyx behaving oddly afterwards (power off, wait 10 s, then follow `ERISTA_T1_RUN.md` section 6);
- a Nyx reading above the provisional 40 C at the start of the second attempt;
- any result the sheet does not describe: send it, do not explain it away.

## 11. Record

Report the video, the Nyx start and end figures (start before selecting the entry), the selection-to-menu time, and a photograph of the loader screen
sharp enough to read every digit. Transcribe the words from the photograph, not from this sheet. The record goes in `boot/diag/erista-t2-run1.txt`, the
result in `ERISTA_T2_RESULT.md`, as for the earlier runs.
