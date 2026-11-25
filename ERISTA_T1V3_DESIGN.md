# T1 v3: slow I2C1 with the controller divisor only

Implemented 2026-11-26, revised after review the same day. Not built on the Mac and not run. T1 v3 hardware boot, T2 to T4,
D1 hardware boot and T1-H stay on hold. `boot` stays 0. No clock, PMIC, voltage, EMC or firmware change is part of it. The CAR
is read and never written.

Entry `iOS Diag T1V3` (`//boot:diag_t1v3`, `ios-diag-t1v3`, ident `iDiagT1V3`, define `SWITCH_DIAG_T1V3`). Sources
`boot/diag/loader/t1v3*.c`. D0, D0-R, D1, T1 v2 and T1-R keep their own files and their images are byte for byte as recorded.

## What it tests

T1 v2 got no answer from the TMP451 on I2C1. T1-R then found `CLK_SOURCE_I2C1` at `00000003`, which with the Linux clock model
is `pll_p / 4`, where hekate wrote `C0000003` (`clk_m / 4`, 4.8 MHz). With the divisor at hekate's 5 the SCL would be far above
what a fast-mode part accepts. T1 v3 slows the controller by its own divisor and repeats one ID read. An ACK supports the
speed hypothesis and says nothing about who changed the source. A NOACK says the speed was not the only problem.

## Decisions

| Item | Value |
| --- | --- |
| Divisor field (bits 31:16) | 255, word `00FF0001`. Written once, never restored. |
| Apply | `CONFIG_LOAD` `0x01`. The two transfers keep hekate's `0x25`. |
| Gate | read only, listed below. Any miss stops before the first write. |
| Timing words | pinned to what T1-T read on both controllers: `TIMING_0` `00000204`, `TIMING_1` `04070404` (`SWITCH_T1V3_TIMING_0_PIN`, `_1_PIN` in `t1v3.h`). A word that differs in any bit stops with `0607` before the first write. T1 v3 writes neither register. |
| ID read | once: one pointer write and one read. No retry. |
| CAR | four reads, no write |

Rate model: `SCL = module clock / ((TLOW + THIGH + 2) x (divisor + 1))`. The 102 MHz module clock is the Linux model applied to
measured words and is unverified.

## Review answers

Sources: Linux `drivers/i2c/busses/i2c-tegra.c` and FreeBSD `sys/arm/nvidia/tegra_i2c.c` (fetched 2026-11-26), the Linux
`i2c: tegra: Add I2C interface timing support` patch thread, hekate `bdk/soc/i2c.c` at v6.5.5, and the T1-R readings.

### 1. PLLP_BASE

The first design checked that `PLLP_BASE` decodes to 408 MHz. I dropped it in the revision for three reasons: the check is a
frequency model and not a property of the write; the T1-R transcription marked the last digit of `48115408` as not legible, and
that digit sits in the M field; and the approved list did not name it. None of that removes the premise. The divisor choice rests
on the rate model, and the model rests on the PLLP fields. The check is back as a read-only gate on the fields, not on a frequency:
M 8, N 85, P 1 (M bits 7:0, N bits 17:10, P bits 24:20, the Linux table row for 38.4 MHz to 408 MHz). Other bits are ignored. A
miss stops with `061B` before any I2C1 register is read. The raw word and the three fields are shown. A misread digit costs a
re-run, not a risk.

### 2. INTERFACE_TIMING_1 and INTERFACE_TIMING_0

| Register | Field | Bits | Source |
| --- | --- | --- | --- |
| `TIMING_0` (0x94) | `TLOW` | 5:0 | Linux |
| | `THIGH` | 13:8 | Linux |
| `TIMING_1` (0x98) | `TSU_STA` | 5:0 | Linux |
| | `THD_STA` | 13:8 | Linux |
| | `TSU_STO` | 21:16 | Linux |
| | `TBUF` | 29:24 | Linux |

No source read gives a reserved bit, a unit, or a reset value. Bits 31:14 and 7:6 of `TIMING_0`, and 31:30, 23:22, 15:14 and 7:6
of `TIMING_1`, are only outside every field mask. FreeBSD defines no bits for either register. What the sources do say:

- Linux writes `TIMING_0` on T210 with `TLOW` 4 and `THIGH` 2 (`00000204`) for every mode. That is the value Linux programs. It is
  not a reset value. My earlier text called `00000204` the known T210 value and gated on it; that was wrong, and the gate no
  longer treats it as expected.
- Linux writes `TIMING_1` only when the per-SoC `setup_hold_time` is non-zero, and the T210 entries are 0. Its comment says
  "Otherwise, preserve the chip default values". So `TIMING_1` on T210 holds a chip default that no source states.
- The VI instance init in Linux writes `TBUF` 4, `TSU_STO` 7, `THD_STA` 4, `TSU_STA` 4 (`04070404`). That is another
  controller instance and not evidence for I2C1.
- The earlier readability claim: Linux's `i2c_writel` reads each register back after writing it (its comment: "read back
  register to make sure that register writes completed"). That shows the registers exist and read back what was written. It does
  not show what an untouched register holds.

So neither expected value can be decided from what was read. The gate reads both words, stops on 0 (`0606`) and on all ones
(`0608`), and then accepts a word only when it equals its pin. A pin of 0 still stops with `061A`; the production pins are set
(see below).

#### Read-only diagnostic (implemented as T1-T, `ERISTA_T1T_DESIGN.md`; first run in `ERISTA_T1T_RESULT.md`)

T1-T ran on 2026-11-26: both controllers read `TIMING_0` `00000204` and `TIMING_1` `04070404`. The review approved them as the pins
on 2026-11-26 (04:00 JST). The rate recomputed from the measured words and the open `CONFIG_LOAD` items are in the result file;
the run sheet is `ERISTA_T1V3_RUN.md`.

T1-T, an independent entry in the T1-R pattern: no store function linked, the inspection requires a single-load raw read with
one caller, and the script adds nothing to D0's commands.

- Reads: `RST_DEVICES_L` and `H`, `CLK_OUT_ENB_L` and `H`, `PLLP_BASE`; and for I2C1 (`0x7000c000`) and I2C5 (`0x7000d000`)
  `CNFG`, `STATUS`, `INT_STATUS`, `CLK_DIVISOR`, `CONFIG_LOAD`, `TIMING_0`, `TIMING_1`. A block is read only when its clock bit is
  on and its reset bit is clear (I2C1: L bit 12, I2C5: H bit 15).
- Why two blocks: hekate sets up both and writes neither timing register. Equal words on two controllers support the idea that
  they are the reset defaults. Different words leave it open. Neither proves it.
- Use: the words go into the review. Pins are set only from them, and a zero or all-ones word or a non-zero reserved bit ends the
  idea of running T1 v3.
- Risk class: the same as T1-R's five I2C1 reads, at two offsets inside the same page. A read can still stall the CPU, and no
  software limit ends that.
- Alternative: run `iOS Diag T1V3` as built. With empty pins it makes only reads before the stop, but it is not independent, it
  reads the I2C1 block only, and its first run is the hardware boot that stays on hold.

### 3. CONFIG_LOAD and bit 2

| Bit | Name | Source |
| --- | --- | --- |
| 0 | `MSTR_CONFIG_LOAD` | hekate, FreeBSD, Linux |
| 1 | `SLV_CONFIG_LOAD` | FreeBSD |
| 2 | `TIMEOUT_CONFIG_LOAD` | hekate, FreeBSD |
| 5 | none | hekate writes `BIT(5)` with the other two |

Linux writes bit 0 only and waits for the whole register to read 0. T1-R read `00000004` at rest after hekate's `0x25` writes.
Bit 0, and bit 5 if it is a strobe, were clear; bit 2 stayed set. So on this silicon bit 2 does not clear by itself, which
Linux's wait would not tolerate. What a write of `0x01` does to it is not documented: it may stay (write-one semantics) or clear
(plain bit). What the later `0x25` does follows from the same: it writes bit 2 as 1, and `00000004` after it is what hekate left.
Whether a pending bit 2 changes how the divisor is applied is not known. I do not call either outcome safe. The design accepts
both words after the apply and shows both before and after (`0 or 4` for the apply, exactly `4` before each go).

### 4. When CONFIG_LOAD is not as expected

| Point | Condition | Stop |
| --- | --- | --- |
| Before write 1 | bit 0 set | `0616` |
| Before write 1 | word is not `00000004` | `0615` |
| After write 2 | bit 0 still set after 1 ms | `060A` |
| After write 2 | word is neither `00000000` nor `00000004` | `0617` before any transfer |
| After each `0x25` load | bit 0 still set after 1 ms | `060A` |
| After each `0x25` load | word is not `00000004` | `0617` before the go write |

No transfer starts from a state the table does not list.

### 5. BUSY never observed

Nothing read separates "finished before the first poll" from "never started" in normal mode. The `INT_STATUS` packet-complete bits
that Linux and FreeBSD use belong to packet mode. hekate's normal mode transfers do not read `INT_STATUS`; only its bus clear in `i2c_init` does. `CNFG` bit 9 (hekate's
`NORMAL_MODE_GO`, FreeBSD's `SEND`) read 0 at rest in T1-R (`00002842`) after hekate's use of the bus, which fits a self-clearing bit, but
clearing could mean "latched" and not "finished". So no completion evidence exists besides `BUSY` itself.

The 250 us settle is removed. After the go write the loader polls `STATUS` until `BUSY` rises, for at most 1000 us, then until
it clears (100 ms). `BUSY` never seen: stop `0618`, and the transaction is not counted as done and its NOACK bits are not read.
`CNFG` and `INT_STATUS` are read once after each go and shown, and decide nothing. If the real SCL is faster than the model, the
likely result is `0618` or a very short start time, and either is a finding.

### 6. A stale CMD_DATA1 taken for the ID

The ID is accepted only when the read transaction showed `BUSY`, `CMD_DATA1` read before the read was not `55`, and it reads `55`
after. A byte already equal to `55` stops the run with `0619` before the read. Otherwise the value must have changed to `55`
during a transaction that started, so a stale `55` cannot pass; a stale pointer `FE` ends as `060E`. Not known: that `CMD_DATA1`
reads back the pointer written to it. The check does not need it, because it compares before and after.

### 7. CONFIG_LOAD bit 0 before the apply

Needed. A set bit 0 would mean a load is still in flight when write 2 arrives. Gate `0616`, and the whole word must be
`00000004` (`0615`).

### 8. No write after an anomaly

`put()` makes no write once the result holds a stop code. A stop can be raised only after a load poll (after write 2, 6 or 10)
or after a go (write 7 or 11); writes 3 to 5 and 8 and 9 run back to back with no check between them. The host test lists every
stop, the exact number of writes made and that the writes equal the first n words of the normal sequence; the counts are
2, 6, 7, 10 and 11. The guard in `put()` is not reachable by a host test (no outside input can set a stop between two writes), so
a source test holds it in place; removing it was not caught by any behavioral test.

### 9. Static checks on the linked binary

| Level | Check | Where it runs |
| --- | --- | --- |
| Sources | register offsets, write list, one load and one store, loop caps, pins equal the T1-T fixture | `diag_t1v3_test.py` |
| Linked ELF | symbols in the map, raw accesses one instruction each and called only by the gate, device immediates | `diag_loader.py link`, which the Bazel rule runs on the final ELF; a failure fails the build |
| Flat image | every word in the CAR or I2C1 page is one of the 13 listed addresses, once | `inspect_image`, called by the same link step and by `diag_images.py` |

The real `diag_loader.py link --t1v3` returned 0 on the T1 v3 objects and 1 with an extra object holding the word `0x60006008`
("T1 v3 image holds the address 0x60006008, which is not listed"). The Bazel build itself has not run on this machine.

## Write audit

The write allowlist (`t1v3_regs.c`) holds exactly these registers and values. A refusal stops the run (`0610`). The normal run
makes eleven writes in this order.

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

`0x25` is bits 5, 2 and 0. Bit 0 is the load strobe, bit 2 is named `TIMEOUT_CONFIG_LOAD` in hekate, bit 5 has no name in
anything read. T1 v2 writes the same `0x25` before each of its ten transfers. T1 v3 keeps `0x25` for the transfers because it is
what hekate uses for them, and `0x01` for the divisor because it is what Linux uses for that.

## Sequence and stops

1. The counter must advance, or `0612` with nothing read.
2. CAR reads. Clock off `0601`, in reset `0602`, source not `00000003` `0603`, `PLLP_BASE` fields not 8, 85, 1 `061B`.
3. I2C1 reads: `CNFG`, `STATUS`, `INT_STATUS`, `CLK_DIVISOR`, `CONFIG_LOAD`, `TIMING_0`, `TIMING_1`. `BUSY` `0604`, `INT_STATUS`
   not 0 `0613`, divisor not `00050001` `0605`, `CONFIG_LOAD` `0616` or `0615`, timing `0606`, `0608`, `061A` or `0607`.
4. Three samples of `STATUS` and `INT_STATUS`, 100 us apart, all idle and equal to the first read, else `0609`.
5. Writes 1 and 2, poll `CONFIG_LOAD` bit 0 for at most 1 ms (`060A`), read back. Divisor not `00FF0001` `060B`, load word
   `0617`, `BUSY` `060F`, `INT_STATUS` changed `0614`.
6. Writes 3 to 11 as two transactions: load check (`060A`, `0617`), go, `BUSY` must rise within 1 ms (`0618`) and clear within
   100 ms (`060C`), NOACK `060D`. Between them `CMD_DATA1` must not hold `55` (`0619`). ID not `55` `060E`.
7. `STATUS` and `INT_STATUS` once more, and the screen.

Every stop ends the work at once: no retry, no bus clear, no controller reset, no restore. Gate stops write nothing. After
`060A`, `060B` or `0617` at the apply the divisor stays as written; hold the power button if the screen hangs, and hekate's next
`hw_init` rewrites it.

Every wait and poll loop also ends after 1,000,000 iterations, so a stopped timer ends it with `0612`, `060A`, `060C` or `0618`.

## Display

Rows 9 to 20: CAR words, `PLLP_BASE` with its three fields, the before words, `TIMING_0` and `TIMING_1`, the after words with
the divisor written and not restored, the `CONFIG_LOAD` words after each `0x25`, start and clear times and the words read after
each go, the ID (hex only), the stop code and name, the write and transfer counts. Times are reference values and not an SCL
measurement. The probe record holds the same values.

## Checks

| Check | Where |
| --- | --- |
| Normal run, the exact write sequence, one divisor write, one ID read | `t1v3_test.cpp` |
| Gate: clock, reset, source, `PLLP_BASE` fields, `BUSY`, `INT_STATUS`, divisor, `CONFIG_LOAD`, timing, pins, idle | `t1v3_test.cpp` |
| Each stop after write 1: stop code, exact writes, go count, transfers, no CAR write | `t1v3_test.cpp` |
| Stopped timer at three points; start and busy waits end by count | `t1v3_test.cpp` |
| Static, source and linked image | `diag_t1v3_test.py`, `--t1v3` inspection, `inspect_image` |
| The other six images unchanged | `diag_t1v3_test.py` against `boot/diag/erista-diag-image-hashes.txt` |

## Mac

```
bazel test //boot/diag/loader/test:t1v3_test //boot/diag/loader/test:t1_test //boot/diag/loader/test:t1r_test \
  //tools/package/test:diag_t1v3_test //tools/package/test:diag_t1_test //tools/package/test:diag_t1r_test \
  //tools/package/test:diag_loader_test
bazel build //boot:diag_t1v3
shasum -a 256 bazel-bin/boot/diag_t1v3/package/switchroot/ios-diag-t1v3/*
```

The recorded image hashes come from Ubuntu clang 18.1.3 and compare only with that compiler. On the Mac, build the five
existing images before and after the change with the same compiler and compare:

```
git worktree add ../before afe244268
(cd ../before && bazel build //boot:diag_loader //boot:diag_loader_r //boot:diag_loader_sd //boot:diag_t1 //boot:diag_t1r)
bazel build //boot:diag_loader //boot:diag_loader_r //boot:diag_loader_sd //boot:diag_t1 //boot:diag_t1r
shasum -a 256 ../before/bazel-bin/boot/diag_*/package/switchroot/*/* bazel-bin/boot/diag_*/package/switchroot/*/*
```

## Unverified

- The TMP451 ACKs in shutdown (the data sheet is silent).
- The values of `TIMING_0` and `TIMING_1` on this silicon, their reserved bits and their units.
- What `0x01` does to `CONFIG_LOAD` bit 2, what bit 5 is, and whether a pending bit 2 changes how a load is applied.
- That `BUSY` rises during a normal mode transaction and what `CNFG` bit 9 and `INT_STATUS` do in it.
- The module clock is 102 MHz, and that `PLLP_BASE` was transcribed correctly.
- The 1 ms load bound at divisor 255.
- The writer of the changed sources (`ERISTA_HEKATE_BOOTPATH.md`). The test does not look for it.
- Other devices on I2C1 (charger, fuel gauge, BM92T36) see the slower bus only after write 2. No other master is known in the
  loader window; that is a reading of code that was not run.

## For the reviewer

The pins are set. T1 v3 stays on hold until the run sheet is approved. The review of the open items is in `ERISTA_T1V3_RUN.md`
section 9.
