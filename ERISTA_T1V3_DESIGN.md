# T1 v3: slow I2C1 with the controller divisor only

Implemented 2026-11-26 on the review's conditions, not built on the Mac and not run. T1 v3 hardware boot, T2 to T4, D1
hardware boot and T1-H stay on hold. `boot` stays 0. No clock, PMIC, voltage, EMC or firmware change is part of it. The CAR
is read and never written.

Entry `iOS Diag T1V3` (`//boot:diag_t1v3`, `ios-diag-t1v3`, ident `iDiagT1V3`, define `SWITCH_DIAG_T1V3`). Sources
`boot/diag/loader/t1v3*.c`; D0, D0-R, D1, T1 v2 and T1-R keep their own files and their images are byte for byte as recorded.

## What it tests

T1 v2 got no answer from the TMP451 on I2C1. T1-R then found `CLK_SOURCE_I2C1` at `00000003`, which with the Linux clock model
is `pll_p / 4`, where hekate wrote `C0000003` (`clk_m / 4`, 4.8 MHz). With the divisor at hekate's 5 the SCL would be far above
what a fast-mode part accepts. T1 v3 slows the controller by its own divisor and repeats one ID read. An ACK supports the
speed hypothesis and says nothing about who changed the source. A NOACK says the speed was not the only problem.

## Decisions

| Item | Value | Why |
| --- | --- | --- |
| Divisor field (bits 31:16) | 255, word `00FF0001` | at most about 199 kHz for any timing values, about 49.8 kHz with `TLOW + THIGH + 2` = 8 at 102 MHz |
| Apply | `CONFIG_LOAD` = `0x01` | the bit Linux writes; first candidate |
| Restore | none | the next hekate `hw_init` rewrites the divisor |
| Timing registers | stop before any write if unexpected | see the gate |
| ID read | once | one write transaction and one read transaction, no retry |
| CAR | reads only | three words |

Rate model: `SCL = module clock / ((TLOW + THIGH + 2) x (divisor + 1))`. The 102 MHz module clock is the Linux model applied to
a measured word and is unverified; the gate therefore checks the source word, not a frequency.

## Premise checks

| # | Question | Result |
| --- | --- | --- |
| 1 | What does `BUS_CLEAR_STATUS` bit 0 say? | A result: set means the last bus clear succeeded (Linux `tegra_i2c_issue_bus_clear`). It cannot show "a clear is in progress", so the gate does not use it. The register is not in the allowlist. |
| 2 | Can `INTERFACE_TIMING_0/1` be read, and what does a read do? | Linux `i2c_writel` reads back every register it writes (not `TX_FIFO`), so the two are read on T210 with the value discarded. No read side effect is documented in anything read. This is the only evidence; it is Linux behavior, not a measurement here. `TIMING_0` has a known T210 value (`TLOW` 4, `THIGH` 2, `00000204`); no source gives `TIMING_1` on this silicon. |
| 3 | Continue on 0, unexpected or unreadable timing? | No. Stop `0606`, `0607` or `0608` before the first write. `TIMING_0` must read exactly `00000204`. `TIMING_1` stops on 0 and on `FFFFFFFF` and is otherwise recorded and shown. |
| 4 | Effect of a `CONFIG_LOAD` write of `0x01` on the other bits? | Unknown. Linux writes `0x01` and polls the whole register to 0; this silicon read `00000004` at rest, so bit 2 is set. If bit 2 is a stored setting, `0x01` clears it (hekate writes `0x25`). The loader shows `CONFIG_LOAD` before and after the write so the effect is recorded. Not verified. |
| 5 | All writes, including T1 v2's `0x25` | Below. |
| 6 | Waits for divisor 255 | Below. |
| 7 | Transfer time | Reference only. |
| 8 | Loop caps against a stopped timer | Below. |

## Write audit

The write allowlist (`t1v3_regs.c`) holds exactly these registers and values; anything else is refused before the address is
used, and a refusal stops the run (`0610`). The normal run makes eleven writes in this order.

| # | Register | Value | Purpose |
| --- | --- | --- | --- |
| 1 | `CLK_DIVISOR` | `00FF0001` | the one change |
| 2 | `CONFIG_LOAD` | `01` | applies it |
| 3 | `CMD_ADDR0` | `98` | TMP451 write address |
| 4 | `CMD_DATA1` | `FE` | register pointer: manufacturer ID |
| 5 | `CNFG` | `2800` | write, one byte, no go |
| 6 | `CONFIG_LOAD` | `25` | as T1 v2 and hekate |
| 7 | `CNFG` | `2A00` | go |
| 8 | `CMD_ADDR0` | `99` | read address |
| 9 | `CNFG` | `2840` | read, no go |
| 10 | `CONFIG_LOAD` | `25` | as T1 v2 and hekate |
| 11 | `CNFG` | `2A40` | go |

T1 v2's own writes are the same transfer sequence with `CONFIG_LOAD` `0x25` and ten transactions. `0x25` is bits 5, 2 and 0.
Bit 0 is the load strobe; what bits 5 and 2 do is not documented in anything read, so `0x25` stays unverified. T1 v3 keeps `0x25`
for the two transfers because it is what hekate uses for the same transfers, and `0x01` for the divisor because it is what
Linux uses for the same change. The two values differ on purpose; the review decides whether the transfers should use `0x01` as well.

## Sequence and stops

1. The counter must advance, or stop `0612` with nothing read.
2. CAR reads. Clock off: `0601`. In reset: `0602`. Source not `00000003`: `0603`. No I2C1 register is read before these pass.
3. I2C1 reads: `CNFG`, `STATUS`, `INT_STATUS`, `CLK_DIVISOR`, `CONFIG_LOAD`, `TIMING_0`, `TIMING_1`. `BUSY` set: `0604`. `INT_STATUS`
   not 0: `0613`. Divisor not `00050001`: `0605`. Timing: `0606`, `0607`, `0608`.
4. Three samples of `STATUS` and `INT_STATUS`, 100 us apart, all idle and equal to the first read: else `0609`.
5. Writes 1 and 2, then poll `CONFIG_LOAD` bit 0 for at most 1 ms: else `060a`. Read back `CONFIG_LOAD`, `CLK_DIVISOR`,
   `STATUS`, `INT_STATUS`. Divisor not `00FF0001`: `060b`. `BUSY`: `060f`. `INT_STATUS` changed: `0614`.
6. Writes 3 to 11, with one go write per transaction. `BUSY` for over 100 ms: `060c`. NOACK: `060d`. ID not `55`: `060e`.
7. Read `STATUS` and `INT_STATUS` again and show everything.

Every stop ends the work at once. There is no retry on NOACK, BUSY or a `CONFIG_LOAD` anomaly, no bus clear, no controller reset
and no restore of the divisor. Gate stops write nothing. `060a` and `060b` leave the divisor as written; hold the power button
if the screen hangs, and hekate's next `hw_init` rewrites it.

The gate adds two checks to what the review listed: the source word (kept from the first design) and `INT_STATUS` equal to 0.
It has no `BUS_CLEAR_STATUS`, `FIFO_STATUS` or PLLP condition.

## Waits for divisor 255

| Wait | Value | Reading |
| --- | --- | --- |
| Settle after go | 250 us | One transaction is about 20 SCL clocks: about 400 us at 49.8 kHz, about 100 us at the fastest the divisor allows. The first `BUSY` poll lands inside a slow transfer. It is not a completion test. |
| `BUSY` bound | 100 ms | Over 200 times the expected 400 us. The decision is `BUSY` clear, with no assumption about the rate. |
| `CONFIG_LOAD` bound | 1 ms | Linux's bound. With divisor 255 the load could take longer if it waits for SCL; 1 ms is about 50 clocks at 49.8 kHz. A timeout stops. Unverified. |
| Idle sample spacing | 100 us | One short wait between three reads. |

Elapsed time runs from the go write to the poll that saw `BUSY` clear. It cannot be below the 250 us settle, so it does not
separate rates faster than that. It is shown as a reference, with whether `BUSY` was seen, and is not an SCL measurement.

Every wait and poll loop also ends after 1,000,000 iterations, so a stopped timer ends it with `0612` (settle and idle waits),
`060a` or `060c`. The counter check in step 1 only proves the timer ran at the start.

## Display

Rows 9 to 16: CAR words, the before words, `TIMING_0` and `TIMING_1`, the after words with the divisor written and not
restored, the two transfer times and whether `BUSY` was seen, the ID, the stop code and name, and the write and transfer
counts with `STATUS` and `INT_STATUS` after. The ID is printed as hex only. The probe record holds the same values.

## Checks

| Check | Where |
| --- | --- |
| Normal run, exact write sequence, one divisor write, one ID read | `t1v3_test.cpp` |
| Gate mismatch, timing abnormal, idle not satisfied, `CONFIG_LOAD` timeout, divisor readback, NOACK, `BUSY` timeout, stopped timer at three points | `t1v3_test.cpp` |
| No write beyond the stop, no CAR write, writes only allowlisted words | `t1v3_test.cpp` |
| Static: one load and one store function, one caller, no device address outside `t1v3_regs.c`, reviewed offsets and values | `diag_t1v3_test.py`, the loader inspection (`--t1v3`) |
| The other five images unchanged | `diag_t1v3_test.py` against `boot/diag/erista-diag-image-hashes.txt` |

## Mac

```
bazel test //boot/diag/loader/test:t1v3_test //boot/diag/loader/test:t1_test //boot/diag/loader/test:t1r_test \
  //tools/package/test:diag_t1v3_test //tools/package/test:diag_t1_test //tools/package/test:diag_t1r_test \
  //tools/package/test:diag_loader_test
bazel build //boot:diag_t1v3
shasum -a 256 bazel-bin/boot/diag_t1v3/package/switchroot/ios-diag-t1v3/*
```

The recorded image hashes come from Ubuntu clang 18.1.3 and are only comparable with that compiler. On the Mac, build the
five existing images before and after the change with the same compiler and compare:

```
git worktree add ../before afe244268
(cd ../before && bazel build //boot:diag_loader //boot:diag_loader_r //boot:diag_loader_sd //boot:diag_t1 //boot:diag_t1r)
bazel build //boot:diag_loader //boot:diag_loader_r //boot:diag_loader_sd //boot:diag_t1 //boot:diag_t1r
shasum -a 256 ../before/bazel-bin/boot/diag_*/package/switchroot/*/* bazel-bin/boot/diag_*/package/switchroot/*/*
```

Each file must match its counterpart.

## Unverified

- The TMP451 ACKs in shutdown (the data sheet is silent).
- `INTERFACE_TIMING_0/1` read as the model assumes; `TIMING_1` has no known value.
- The module clock is 102 MHz.
- `CONFIG_LOAD` bits 5 and 2, and what `0x01` does to bit 2.
- The writer of the changed sources (`ERISTA_HEKATE_BOOTPATH.md`). The test does not look for it.
- Other devices on I2C1 (charger, fuel gauge, BM92T36) see the slower bus only after write 2. No other master is known in the
  loader window; that is a reading of code that was not run.

## For the reviewer

Decide whether the two transfers use `0x25` or `0x01`, whether `TIMING_1` may stay unchecked beyond 0 and all ones, and whether
the two added gate checks stay. The hardware boot is not started.
