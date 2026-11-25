# T1 v3 result: first hardware run

Run 2026-11-26 on the Erista (`T210B01=0`, `SKU=0`, `BOOT=0`), hekate 6.5.5, from Nyx "More configs", commit `f8cc80dcc` built on the
Mac. The user reported it at 08:37 JST with a photograph of the loader screen. The screen text agrees with the loader's own output
line for line (`t1v3_hardware_test.cpp`). The photograph is not stored in the repository. The record is
`boot/diag/erista-t1v3-run1.txt`.

The screen showed `T1V3 COMPLETE CODE 0000`, `WRITES 11`, `TRANSFERS 2 OF 2`, `TMP451 0X4C ID(FE) 55`, `DATA1 BEFORE READ FE`,
`BOOT STAYS 0` and the `RESET IN 15 S` line. After the countdown hekate came back by itself. The Nyx temperature before and after, the
video and the selection-to-menu time were not reported. One observation per word.

## What was measured

| Word | Value |
| --- | --- |
| `RST_DEVICES_L`, `CLK_OUT_ENB_L`, `SRC_I2C1`, `PLLP_BASE` | `049102CA`, `9802D131`, `00000003`, `48115408`. All four equal T1-T and T1 v2. |
| Before: `CNFG`, `STATUS`, `INT`, `DIV`, `LOAD` | `00002842`, `0`, `0`, `00050001`, `00000004`. Equal to T1-T's I2C1 words. |
| `TIMING_0`, `TIMING_1` | `00000204`, `04070404`. Equal to the pins. |
| After the apply: `LOAD`, `DIV`, `STATUS`, `INT` | `00000004`, `00FF0001`, `0`, `0` |
| `LOAD` after each `0x25` | `00000004`, `00000004` |
| Pointer write | BUSY seen at 6 us, clear at 433 us; `CNFG` after go `00002800`, `INT` `0` |
| Read | BUSY seen at 5 us, clear at 433 us; `CNFG` after go `00002840`, `INT` `0` |
| ID, `DATA1` before the read | `55`, `FE` |
| `STATUS`, `INT` at the end | `0`, `0` |

Times run from the go write to the poll that saw the event. They are reference values of a polling loop.

## What it shows

- The write path ran as designed: eleven writes, two transfers, no stop, no retry. The divisor readback was `00FF0001`, the bus
  ended idle with no pending interrupt.
- The TMP451 acknowledged 0x4C at the changed divisor and returned `55` after `DATA1` held the pointer `FE`. A stale `55` could not pass:
  the check needs `DATA1` to differ from `55` before the read.
- The gates held on real words: every pinned and expected word read as T1-T had read it.
- A safe-state claim does not follow. One run, one observation per word.

Two stale texts on the screen, not changed: the `TIMING_0` line still ends `061A: NO REVIEWED VALUE`, which was true for the unpinned build,
and the footer says the PSCI reset "is not verified" although hekate returned after T1-R, T1-T and now T1 v3.

## Transfer times and the SCL rate

The model is `SCL = module clock / ((TLOW + THIGH + 2) x (divisor + 1))` with `TLOW` 4, `THIGH` 2 (measured `TIMING_0`), divisor 255 and
a module clock of 102 MHz (PLLP 408 MHz divided by 4; the model is unverified). It gives 49.8 kHz, one clock period of 20.1 us.

Each transaction is 18 bit periods on the wire: the address byte and its ACK, and one data byte with its ACK or NACK. Start and stop add
more. Both transactions have the same bit count and both read 433 us, which is consistent.

| Reading of 433 us | Result |
| --- | --- |
| 18 periods exactly (no start or stop overhead) | 41.6 kHz, implies a 85 MHz module clock |
| 22 periods (about 4 for start and stop) | 50.8 kHz, implies a 104 MHz module clock |
| the model, 49.8 kHz | 21.6 periods, so 3.6 periods of overhead |
| 100 kHz | 22 periods would take 220 us: the run is at most about half that rate |
| 199 kHz (408 MHz with no divider) | 22 periods would take 110 us: excluded |

So the times fit the model within what start, stop and polling latency can explain, and they put the effective rate between roughly
42 and 51 kHz. They do not separate 102 MHz from nearby clocks: the unknown overhead leaves a module clock from about 85 to 104 MHz. They
are a polling-loop measure and not an SCL measurement. BUSY rose at 5 to 6 us, a fraction of one 20 us period, which means the controller
starts the transaction at once and BUSY is not a late flag.

What this adds: because the CAR words equal T1 v2's, the divisor scaling (256 against 6, 42.7 times) is the only change in the clock
chain between the two runs. T1 v2's own rate was not measured. Under the same module clock it was about 2.1 MHz.

## CONFIG_LOAD

| Point | Word |
| --- | --- |
| Before the divisor write | `00000004` |
| After `0x01` and bit 0 clearing | `00000004` |
| After each `0x25` | `00000004`, `00000004` |

- Bit 0 cleared by itself every time; no `060A` stop.
- Bit 2 stayed set after a write of `0x01`, which has no bit 2. So that write did not clear it. H1 (a plain bit that any write without
  bit 2 clears) is not supported for this write. H2 (only hardware clears it) fits, and so does write-one-only or sticky behaviour.
  Which of those it is was not tested.
- I2C5 read bit 2 clear in T1-T, so something does clear it there. It was not a `0x01` write on I2C1. A controller reset or another word
  are not excluded and nothing here selects one.
- Bit 5, written as 1 by `0x25`, reads 0 afterwards. It does not latch.
- Nothing in the gate depends on this: the apply accepts `0` or `4`, each transfer needs exactly `4` and got it.

## T1 v2 NOACK: what this result supports

T1 v2 stopped with NOACK at the first transfer (`STATUS` `00000001`). T1 v3 used the same device address, the same pointer and the same
register words for the transfers, the same CAR state (the pads were not read in either run), and ran at about 42 to 51 kHz instead
of the model's 2.1 MHz.

Supported:
- 0x4C answers at that rate, and the address bytes `0x98` and `0x99` and the normal-mode transfer words are right.
- The pads, the wiring and the pull-ups of I2C1 carry a transfer to the sensor, so none of those caused T1 v2's NOACK.
- The sensor was reachable in the entry state, which T1 v2's analysis could not exclude. Its configuration was not read, so shutdown itself
  is not shown to be ACKed or not: the sensor's state in this run is unknown.
- The TMP451 data sheet (fetched 2026-11-26) lists fast mode as 1 to 400 kHz and high-speed mode as 1 to 2.5 MHz. A bus near 2.1 MHz
  without a high-speed master code is outside the first range. This fits a too fast bus as the NOACK cause, and the success at a lower rate
  fits it too.

Not shown:
- That T1 v2 ran at 2.1 MHz. It is a model applied to the same CAR words.
- That the rate was the only cause. v3 also writes `CONFIG_LOAD` `0x01`, waits for BUSY to rise, and reads `STATUS` differently; v2's first
  `STATUS` poll could run before the transfer began (`ERISTA_T1_REVIEW.md`), though v2's NOACK bit was set by the transfer.
- That 100 kHz or 400 kHz would work. Only one rate has run.

## Remaining

1. T2's design (`ERISTA_T2_DESIGN.md`) is a design only. It stays on hold with T3, T4, D1 and T1-H.
2. The module clock stays a model. A measurement would write the CAR clock-frequency measurement registers; it was set aside.
3. The `TIMING_1` unit is unknown. The successful ACK at the slower rate says nothing about its unit.
4. Writers of I2C1's and I2C5's source words and of I2C5's state are not identified.
5. The Nyx temperature before and after, the video and the selection-to-menu time were not reported.
6. The result is not a safety result. Every value stays a candidate.

Sources: this run's photograph; hekate `bdk/soc/i2c.c`, `bdk/thermal/tmp451.c` v6.5.5; TI TMP451 data sheet; the T1-T record
(`ERISTA_T1T_RESULT.md`); `ERISTA_T1_0507.md`.
