# T1: I2C1 and TMP451 communication check

Entry `iOS Diag T1` (`//boot:diag_t1`, ident `iDiagT1`). Implemented 2026-11-25, built and inspected on Linux only, not built
on the Mac, not run on the console. Its first run needs the review that follows this file. `boot` stays 0. Design context:
`ERISTA_THERMAL_DESIGN.md`.

It is the D0 loader with the memory controller and FDT reports replaced by one check: it reads CAR and I2C1, then reads five
TMP451 registers. It never sets the sensor, the fan, a clock, a voltage or the PWM block. The boot script is D0's, so the
console shows D0's `D0 LOADER DIAGNOSTIC` lines before the loader's screen.

## What it touches

Every device access goes through `switch_t1_access` (`t1_gate.c`), which asks `t1_regs.c` first. A refused access is not made.

| Register | Address | Read | Write |
| --- | --- | --- | --- |
| CAR `RST_DEVICES_L`, `CLK_OUT_ENB_L`, `CLK_SOURCE_PWM`, `CLK_SOURCE_I2C1` | 0x60006004, 10, 110, 124 | yes | no |
| Pinmux I2C1 SCL, SDA pads | 0x700030BC, C0 | yes | no |
| I2C1 `CNFG` | 0x7000C000 | yes | 0x2800, 0x2840, 0x2A00, 0x2A40 |
| I2C1 `CMD_ADDR0` | 0x7000C004 | no | 0x98, 0x99 |
| I2C1 `CMD_DATA1` | 0x7000C00C | yes | 0xFE, 0x03, 0x04, 0x00, 0x01 (register pointers; configuration and rate at their read addresses) |
| I2C1 `STATUS`, `INT_STATUS`, `CLK_DIVISOR` | 0x7000C01C, 68, 6C | yes | no |
| I2C1 `CONFIG_LOAD` | 0x7000C08C | yes | 0x25 |

The sequence is hekate's `_i2c_send_normal` and `_i2c_recv_normal` (`ERISTA_THERMAL_DESIGN.md`, T1). No bus clear, no divisor
write, no FIFO flush, no interrupt clear. I2C1 registers are not touched unless CAR says the clock is on and the block is
out of reset.

Limits: one attempt per transfer, no retry, 100 ms for `BUSY`, 1 ms for `CONFIG_LOAD`, then the stage ends. The time comes from the generic counter (`CNTPCT_EL0`, 19.2 MHz), which is checked to advance before any device access. Every poll also ends after 1,000,000 reads, so a counter that stops later cannot hold the loop. After `go`, the first `BUSY` poll waits 250 microseconds (one byte after the address at 100 kHz takes about 200, from hekate's I2C1 comment; not measured). That wait only keeps the poll from running before the controller starts. It does not show a transfer ended: `BUSY` and the NOACK bits decide, and a data byte read too early is caught only for register 0xFE (the ID check). The screen
shows hex bytes only; the code has no conversion to degrees.

## Screen

Rows 9 to 16: CAR words and the I2C1 and PWM clock and reset state; I2C1 `CNFG`, `STATUS`, `INT`, `DIV`, `LOAD`; the pad
words; manufacturer ID, configuration and rate; the two raw temperature bytes labelled `NOT A TEMPERATURE`; the result
line (`T1 COMPLETE CODE 0000` or `T1 STOPPED CODE 05xx` with the name); the transfer count (10 expected). Then the loader's
usual `RESET IN 15 S`. The same values go to `probe_record` words 55 to 59.

| Code | Meaning |
| --- | --- |
| 0501 | I2C1 clock off, nothing of I2C1 touched |
| 0502 | I2C1 in reset, nothing touched |
| 0503 | `BUSY` set before the first transfer; no bus clear |
| 0504 | `CLK_DIVISOR` is not hekate's 0x00050001; not written |
| 0505 | `CONFIG_LOAD` still pending after 1 ms |
| 0506 | `BUSY` still set after 100 ms |
| 0507 | no ACK from 0x4C |
| 0508 | register 0xFE is not 0x55 |
| 0509 | `BUSY` set after the last transfer |
| 050A | the allowlist refused an access (a loader bug) |
| 050B | not an Erista; no T1 register touched |
| 050C | the counter did not advance, or a wait ended only by its count; no device touched in the first case |

## Expected on the Erista, from the Nyx path

I2C1 clock on and out of reset, `CLK_DIVISOR` 0x00050001, `STATUS` idle, ID 55, configuration C0 (shutdown), rate a value of
the last Nyx session (hekate leaves 02 after `tmp451_init`, 08 is the power-on value). Local and remote bytes are the last
conversion. PWM clock state is recorded for T3. Anything else is a result to report, not a reason to retry.

## Checks

Linux, from the repository root, with LLVM on `PATH`:

```
PYTHONPATH=tools/package:tools/package/test python3 tools/package/test/diag_t1_test.py
PYTHONPATH=tools/package:tools/package/test python3 tools/package/test/diag_loader_test.py
```

`diag_t1_test` runs the inspection on synthetic disassembly and on a real link of the T1 image. The inspection
(`diag_loader_inspect.py --t1`) requires the two raw accesses to be one load and one store each, and every call to them to
sit in `switch_t1_access`; a second caller fails the link. The host test `//boot/diag/loader/test:t1_test` runs the run logic
against a model of the controller and the sensor: every failure code, no I2C1 access when the clock is off, one go write
when a transfer fails, 45 writes on a healthy bus, and an exhaustive pass over the write allowlist. On the Mac:

```
bazel test //boot/diag/loader/test:t1_test //tools/package/test:diag_t1_test //tools/package/test:diag_loader_test
bazel build //boot:diag_t1
shasum -a 256 bazel-bin/boot/diag_t1/package/switchroot/ios-diag-t1/*
```

Record the Mac's `bl33.bin` hash here and in `NOTES.md` before any run. D0, D0-R and D1 images were rebuilt on Linux from
the tree before and after this change and are byte for byte equal (`//boot:diag_loader`, `diag_loader_r`, `diag_loader_sd`
loader images).

## Mac hashes, v2 (commit `86346c224`)

| File on the card | SHA-256 |
| --- | --- |
| `switchroot/ios-diag-t1/bl31.bin` | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` |
| `switchroot/ios-diag-t1/bl33.bin` | `23ac56e88271afb49d09ed5684a8a223d5fb63f280a6d93c99f2b1d5cd5ac87f` |
| `switchroot/ios-diag-t1/script.txt` | `f762d2a30ce57ee1a4f38a96f7e2c2bc8e347669814dedb07d615dec48da01ef` |
| `bootloader/ini/ios-diag-t1.ini` | `c32c631a13ee5f02b3b7b76aa45b23d159f02c22c4ebb4017f4b0ae245576906` |

`bl31.bin` is the pinned one and `script.txt` starts with the same eight characters (`f762d2a3`) as D0's: the script is D0's
template, as intended.

## Before the first run (after review)

- [ ] The card's four files match the table above. Any mismatch: do not run.
- [ ] Experiment card, backed up; the other entries untouched. Battery 100 %, charger out, hard surface, vents clear, no
      case, and the console cool to the touch before starting.
- [ ] Started from Nyx "More configs", the path the earlier diagnostics used.
- [ ] Phone recording the screen from before `More configs` until hekate returns. You stay with the console, a hand near
      the power button.

## Expected screen

U-Boot lines as D0 (`BOARD OK` through `HANDOFF TO THE LOADER`), then the loader's screen:

| Row | Expected | Anything else |
| --- | --- | --- |
| EL1 and board | `EL1 STATE PASS`, `BOARD` Erista, `T210B01=0`, `SKU=0` | `NOT AN ERISTA` (050B) or `NO DEVICE REGISTER WAS READ`: no T1 register was touched |
| CAR | `I2C1 CLOCK ON OUT OF RESET`; PWM clock on or off is recorded for T3 | clock off or in reset: stop 0501 or 0502, nothing of I2C1 touched |
| I2C1 | `DIV 00050001`, `STATUS` with BUSY (0x100) clear; `CNFG`, `INT`, `LOAD` as hekate left them, no expectation | `DIV` different: stop 0504; BUSY set: stop 0503 |
| Pads | `PAD SCL 00000040 SDA 00000040` (hekate writes input enable, 0x40) | other values are reported, not a stop |
| TMP451 | `ID(FE) 55`, `CONFIG(03)` C0 on the Nyx path (80 or 00 if a plain menu or a reset sensor), `RATE(04)` 02 after Nyx's `tmp451_init` (08 is the power-on value) | a second value is a result to report, not a reason to retry |
| Raw | two bytes labelled `NOT A TEMPERATURE` | |
| Result | `T1 COMPLETE CODE 0000`, `TRANSFERS 10 OF 10` | `T1 STOPPED CODE 05xx` with its name |
| End | `RESET IN 15 S`, then hekate | |

## Stop criteria for the run

Hold the power button until the console is off, then report the last line, in these cases:

- the hashes did not match, the console is warm, or Nyx was not the start;
- any stage passes its deadline (D0's table: 20 s to the first line, 10 s per step, 10 s to the loader, 20 s to the report,
  25 s to hekate);
- `COUNTER FREQUENCY OR COUNT NOT AS EXPECTED. NO RESET`, `PSCI SYSTEM_RESET RETURNED`, `EXIT CALLED OUTSIDE EL1` or an
  exception screen;
- the screen goes black or shows garbage, or the console feels hot.

After any stop code 0503, 0505, 0506, 0507 or 0509, or any other abnormal end, power the console off completely (hold the
button, wait ten seconds) before the next thing. The TMP451 is not powered down by the SoC reset, and hekate's next bus clear
is the only recovery T1 relies on. No second T1 run until it is reviewed.

## Longest T1-specific time

T1's own code: the counter check up to 1,000,000 counter reads (well under 0.1 s), ten transfers of about 0.5 ms each when
healthy, and on a failure one wait of at most 100 ms for `BUSY` or 1 ms for `CONFIG_LOAD`, after which the stage ends. If the
counter stops after the check, the count limits end each loop after 1,000,000 device reads, which is about a second per
loop on the assumption of one microsecond per read (not measured); the stage ends at the first failure, so the worst case is
a few seconds. Around it, the D0 stage deadlines apply: 20 + 4 x 10 + 10 + 20 + 25 = 115 s as an upper figure by deadline,
a stall with a person reacting for about 5 s and holding the button for 12 s about 132 s. The 15 s wait is part of it.

## Temperature risk, again

Nothing in T1 raises it: the script, the loader, the 15 s wait and the fan-off state are D0's, which has run on this console
twice (D0 and D0-R, plus the D1-pre pages) and returned to hekate each time. T1 adds register reads and ten I2C transfers
(milliseconds). No temperature is read, so the run is still bounded only by the person and the deadlines. The one new risk
is on the way out: a transfer left unfinished could keep the TMP451 from answering hekate, and Nyx's fan loop then reads no
temperature. That is why a failed run ends in a full power-off. Fan, PWM and rail are not touched.

## What this does not show

- That the model matches the silicon (the controller model is from hekate's driver, not the TRM).
- That a TMP451 byte is a temperature. T1 does not claim it.
- That the pad levels are right; the pad words are read, not judged.
- Anything about the fan, the PWM block, SOC_THERM or the watchdogs.
- That the 250 microsecond wait is long enough on this console, or that the 100 ms bound holds in silicon.

## Revisions

| | Source commit | Mac `bl33.bin` SHA-256 | Notes |
| --- | --- | --- | --- |
| v1 | `15934b805` | not recorded in this repository | Reads TMP451 configuration and rate at 0x09 and 0x0A, the write addresses. Counter checked only at exit. No wait before the first `BUSY` poll. Mac results for v1 are history, not the current artifact. |
| v2 | `86346c224` | `23ac56e88271afb49d09ed5684a8a223d5fb63f280a6d93c99f2b1d5cd5ac87f` | Read addresses 0x03 and 0x04; counter checked first; count limits on every poll; 250 microsecond wait; stop code 050C. Mac build, inspection and the three Bazel test targets pass (reported by the user). Against v1 only `bl33.bin` changed; `bl31.bin`, `script.txt` and the ini are equal. |

`CONFIG_LOAD` stays 0x25, hekate's value: bit 0 (master load) is in Linux, bit 2 (timeout load) is defined there and unused,
bit 5 has no definition in any source read and its meaning is unknown. It is kept because normal mode with this value is
what works on this console, not because the bit is understood.

No bus clear or FIFO flush was added. After a timeout the controller can be left mid-transfer and the reset that follows is
not shown to recover it. hekate's next `i2c_init` bus clear is the expected recovery and has not been observed.

## End of the stage

`exit.c` is shared with D0, D0-R and D1, so it is not changed here. Its 15 s loop ends by counter and is guarded by
`counter_advances` before it starts, but it sleeps in `wfe` and relies on the event stream (`CNTKCTL_EL1`) to wake; if no
event arrives it waits forever. After a failed `PSCI` call or an exception the loader halts on purpose. Every stop code of T1
still reaches this path: the report is drawn, then the wait and the reset. The power-button hold covers the cases where it
does not.

A register read that never returns stalls the CPU. Interrupts and SError are masked in the loader, so nothing ends it and no
software timeout can. The only guard is the CAR check: I2C1 is read only when its clock is on and the block is out of reset.

## Audit before the first run

- Allowlist against the code: the raw read and write are one load and one store, called only from `switch_t1_access`, which is
  called only from `io_read` and `io_write`. No device address is formed in code; all 13 register addresses sit in one 0x68 byte `.rodata`
  table. The exhaustive host test and the exact transfer order test agree with the allowlist.
- Counter stopped: 050C before any device access; if it stops later, the count limits end each loop.
- Transfer failure: one attempt, then the stage ends, status is read, the report is drawn, the 15 s wait and the reset follow.
- Screen values, codes and abort rules are in the sections above.
