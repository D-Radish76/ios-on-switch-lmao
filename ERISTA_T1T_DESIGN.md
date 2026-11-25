# T1-T: read the I2C1 and I2C5 timing registers

Implemented 2026-11-26 on the review's conditions. Not built on the Mac and not run. T1-T hardware boot waits for review. T1 v3,
T2 to T4, D1 hardware boot and T1-H stay on hold. `boot` stays 0. It changes no clock, PMIC, voltage, EMC or firmware.

Entry `iOS Diag T1T` (`//boot:diag_t1t`, `ios-diag-t1t`, ident `iDiagT1T`, define `SWITCH_DIAG_T1T`). Sources
`boot/diag/loader/t1t*.c`, built from T1-R's pattern and sharing nothing with T1 v3. The script is the plain
`u-boot-script.txt`. The other images are byte for byte as recorded in `boot/diag/erista-diag-image-hashes.txt`.

## What it records

`INTERFACE_TIMING_0` and `INTERFACE_TIMING_1` of I2C1 (`0x7000c000`) and I2C5 (`0x7000d000`). No source read gives either
register a reset value, reserved bits or units. Linux writes `TIMING_0` itself and leaves `TIMING_1` at the "chip default".
hekate never writes them. So the words as they stand after hekate are the only evidence to collect.

Per controller it also reads `CNFG`, `STATUS`, `INT_STATUS`, `CLK_DIVISOR` and `CONFIG_LOAD`. From the CAR it reads
`RST_DEVICES_L/H`, `CLK_OUT_ENB_L/H`, `CLK_SOURCE_I2C1` to `I2C6` and `PLLP_BASE`. That is 11 CAR reads and 7 per controller.
Each controller's registers are read only when its clock bit is on and its reset bit is clear. Otherwise the screen says
`NOT READ: CLOCK OFF` or `NOT READ: IN RESET`, and nothing of that controller is touched.

The screen shows every word raw, then its fields: `TLOW` and `THIGH`; `TSU_STA`, `THD_STA`, `TSU_STO` and `TBUF`; the
`CLK_DIVISOR` halves; the `CONFIG_LOAD` bits 0, 2 and 5 and the rest; the `PLLP_BASE` M, N and P. Bits outside the named
fields are printed as their own word. No frequency or rate is calculated. A line says whether the two controllers' timing words
are equal, and that equal words are not proof of a reset value.

## What it cannot do

- It has no store to a device address. The raw access is one `ldr` and a return; the gate is the only caller.
- The allowlist holds 11 CAR words and 7 words per controller. `CMD_ADDR0`, `CMD_DATA1` and `BUS_CLEAR_STATUS` are not on it.
- No I2C transfer starts, so there is no BUSY wait, no timer and no retry.
- `inspect_t1t` runs on the final linked ELF and the flat image. It requires the single-load raw read, one caller, no store on
  the address path, no T1, T1-R or T1 v3 symbols and no write counterpart. The flat image may hold only the listed register
  addresses, each once, in the CAR, I2C1 and I2C5 pages.

## Read side effects and recovery

No source read documents a side effect of reading these registers. That is not a finding that there is none.

- T1-R already read `CNFG`, `STATUS`, `INT_STATUS`, `CLK_DIVISOR` and `CONFIG_LOAD` of I2C1 on this silicon without a visible
  change. `INT_STATUS` is write-1-to-clear in Linux; a plain read is not expected to clear it, and that is unverified. T1-T reads
  it once and never writes it.
- `TIMING_0`, `TIMING_1` and every I2C5 register are read for the first time. A read of a register in a block whose clock
  is gated could raise a bus error; the clock and reset gate exists for that reason.
- If the screen shows `T1T STOPPED`, code `0701` is a loader bug (the allowlist refused a read) and no further read was made.
- If an exception screen appears, the loader prints it and returns to hekate. Photograph it and stop. No register was written, so
  there is no state to restore.
- If the screen freezes, power the console off with the power button held down and boot hekate again. Keep the photographs
  and report it before any other run.
- If any word differs from the T1-R readings (`CLK_SOURCE_I2C1` `00000003`, `CONFIG_LOAD` `00000004`, `INT_STATUS` `0`), report
  it and run nothing else. The writer of those words is still unidentified.

## Use of the values

Values from T1-T are candidates for T1 v3's `timing_0_pin` and `timing_1_pin`, nothing more. A reading does not prove that a
value is a reset value, is safe, or is what a later boot stage would leave. The pins stay empty until a review fills them.

## Mac commands

```sh
bazel test //boot/diag/loader/test:t1t_test //tools/package/test:diag_t1t_test //tools/package/test:diag_t1v3_test --config=ios17
bazel build //boot:diag_t1t --config=ios17
shasum -a 256 bazel-bin/boot/diag_t1t/package/switchroot/ios-diag-t1t/*
```

The flat image hash from `tools/package/diag_images.py` is `c81b53ad...14bc` with clang 18.1.3. The Bazel clang may differ.
