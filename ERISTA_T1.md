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
| I2C1 `CMD_DATA1` | 0x7000C00C | yes | 0xFE, 0x09, 0x0A, 0x00, 0x01 (register pointers) |
| I2C1 `STATUS`, `INT_STATUS`, `CLK_DIVISOR` | 0x7000C01C, 68, 6C | yes | no |
| I2C1 `CONFIG_LOAD` | 0x7000C08C | yes | 0x25 |

The sequence is hekate's `_i2c_send_normal` and `_i2c_recv_normal` (`ERISTA_THERMAL_DESIGN.md`, T1). No bus clear, no divisor
write, no FIFO flush, no interrupt clear. I2C1 registers are not touched unless CAR says the clock is on and the block is
out of reset.

Limits: one attempt per transfer, no retry, 100 ms for `BUSY`, 1 ms for `CONFIG_LOAD`, then the stage ends. The screen
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

## What this does not show

- That the model matches the silicon. The controller model is written from hekate's driver, not from the TRM.
- That a TMP451 byte is a temperature. T1 does not claim it.
- That the pad levels are right; the pad words are read, not judged.
- Anything about the fan, the PWM block, SOC_THERM or the watchdogs.

## Before the first run (after review)

- [ ] Experiment card, console cool, battery charged, charger out, hard surface. The D0 loader's checklist applies:
      `boot/diag/loader/RUN_CHECKLIST.md`.
- [ ] Started from Nyx "More configs", the path the earlier diagnostics used.
- [ ] Mac hashes recorded and equal to the card's.
- [ ] Photograph the screen. Hold the power button at the first sign of a stall.
