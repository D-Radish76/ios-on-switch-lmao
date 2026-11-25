# T1 v3 design: slow I2C1 with the controller divisor only

Design for review. Nothing here is implemented, no register is written, T1 v3, T2 to T4 and the D1 hardware boot stay on
hold, `boot` stays 0. No clock, PMIC, voltage, EMC or firmware change is part of it. This is option 1 of
`ERISTA_T1V3_OPTIONS.md`, worked out. The CAR is read and never written.

## What it tests

T1 v2 got no answer from the TMP451 on I2C1. T1-R then found `CLK_SOURCE_I2C1` at `00000003`, which with the Linux clock
model is `pll_p / 4` = 102 MHz, where hekate wrote `C0000003` (`clk_m / 4` = 4.8 MHz). With the controller's divisor at the
hekate value 5 the SCL would be about 2.1 MHz, above what a fast-mode part accepts. T1 v3 slows the controller down by its
own divisor and repeats the one ID read. An ACK supports the speed hypothesis. It says nothing about who changed the
source. A NOACK at the slower speed says the speed was not the only problem.

## Facts used

| Fact | Source |
| --- | --- |
| I2C1 block at `0x7000c000`; `CNFG` 0x00, `STATUS` 0x1c (`BUSY` bit 8), `INT_STATUS` 0x68, `CLK_DIVISOR` 0x6c, `BUS_CLEAR_STATUS` 0x88, `CONFIG_LOAD` 0x8c, `INTERFACE_TIMING_0/1` 0x94, 0x98 | hekate `i2c.c`, Linux `i2c-tegra.c` (fetched 2026-11-26) |
| `CLK_DIVISOR`: standard and fast divisor in bits 31:16, high-speed divisor in bits 15:0 | Linux `GENMASK(31, 16)`, `GENMASK(15, 0)` |
| `INTERFACE_TIMING_0`: `TLOW` bits 5:0, `THIGH` bits 13:8. `_1`: `TSU_STA` 5:0, `THD_STA` 13:8, `TSU_STO` 21:16, `TBUF` 29:24 | Linux |
| T210 values Linux writes: `TLOW` 4, `THIGH` 2, divisor 0x19, `has_config_load_reg`, `has_interface_timing_reg`, no Hs support | Linux `tegra210_i2c_hw` |
| `CONFIG_LOAD`: Linux writes `MSTR_CONFIG_LOAD` (bit 0) after the divisor and the timing words and polls up to 1000 us for the register to read 0 | Linux `tegra_i2c_wait_for_config_load` |
| hekate writes `0x25` (bits 5, 2, 0) and waits up to 20 us for bit 0 to clear | hekate `_i2c_load_cfg_wait`, also in the unpacked release (`i2c_init` at `0x4000d1f0`, load at `0x4000d070`) |
| hekate's `i2c_init` stores `0x00050001` in `CLK_DIVISOR` and never writes `INTERFACE_TIMING_0/1` | hekate source and unpacked release |
| T1-R read on I2C1: `CNFG` 00002842, `STATUS` 0, `INT` 0, `DIV` 00050001, `LOAD` 00000004 | `boot/diag/erista-t1r-run.log` |
| Linux has no bus idle check in `tegra_i2c_init`; hekate waits on `STATUS.BUSY` after a transfer | Linux, hekate |

Two things follow from `LOAD` reading 4: bit 0 clears by itself when the load is done, and the register does not read 0 at
rest on this silicon, so a poll for the whole register to be 0 (Linux's form) would time out. T1 v3 polls bit 0 only.

## Rate model

`SCL = module clock / ((TLOW + THIGH + 2) x (divisor + 1))`. Linux asks for the module rate `bus x (TLOW + THIGH + 2) x
(divisor + 1)` with `TLOW` 4 and `THIGH` 2, and hekate's own comment "4.8 MHz -> 100 KHz" agrees with divisor 5. The module
clock is 102 MHz only if the source word means what Linux says and `PLLP_BASE` decodes to 408 MHz (M 8, N 85, P 1). Neither
the timing registers nor the actual SCL have been measured.

| Divisor field | SCL at 102 MHz, `TLOW + THIGH + 2` = 8 | SCL if `TLOW + THIGH + 2` = 2 (smallest) |
| --- | --- | --- |
| 5 (now) | 2.125 MHz | 8.5 MHz |
| 127 | 99.6 kHz | 398 kHz |
| 255 | 49.8 kHz | 199 kHz |

Divisor 127 stays at or below 400 kHz for any `TLOW` and `THIGH`, so the choice does not rest on the assumed reset values. It
is the proposed value. 255 is the alternative if the review wants more margin; a slower bus makes the transfer longer, which
matters only against the provisional human deadlines of the earlier runs and the SMBus timeout of other parts (none known on this bus at idle).

## Writes, listed for review

| # | Register | Value | Why |
| --- | --- | --- | --- |
| W1 | `0x7000c06c` `CLK_DIVISOR` | `(127 << 16)` plus the old low 16 bits | the one change; the high-speed field is kept |
| W2 | `0x7000c08c` `CONFIG_LOAD` | `0x00000001` | applies W1 (bit 0 only, as Linux; hekate's `0x25` also reloads the timeout setting, which this test does not need) |

Everything else is reads and the same single TMP451 ID transaction as T1 v2 (address, count limits, 250 us wait and stop code
scheme unchanged; new stop codes are allocated at implementation, none of v2's reused). No CAR store, no reset or gate bit, no
bus clear, no timing register write. The display and the T1 v2 result screen code are the only other state the loader writes.

## Sequence

1. Read only gate (same CAR words as T1-R, plus the I2C1 block). All of these must hold, or the loader shows them and
   stops with no write:
   - `CLK_SOURCE_I2C1` is `00000003`; `RST_DEVICES_L` and `CLK_OUT_ENB_L` bit 12 show I2C1 enabled and out of reset;
   - `PLLP_BASE` decodes to 408 MHz;
   - `CLK_DIVISOR` is `00050001`;
   - `STATUS.BUSY` is 0, `INT_STATUS` is 0, and `BUS_CLEAR_STATUS` bit 0 reports no bus clear running.
2. Read `INTERFACE_TIMING_0/1` and record them. Decision: if `TLOW + THIGH + 2` reads as 8, continue. If the registers read
   other values, continue only when `(TLOW + THIGH + 2) x 128` is at least 255; the divisor stays 127 either way. If they read
   0 or the read faults, continue (127 is safe for any values) and mark the SCL estimate unknown on the screen.
3. Idle check: three reads of `STATUS`, `INT_STATUS` and `FIFO_STATUS` about 100 us apart, all idle and equal. Any difference
   stops the run before W1.
4. W1, then W2, then poll `CONFIG_LOAD` bit 0 until it clears, at most 1000 us by the hardware timer. Bit 0 still set: stop
   with no further write and no restore.
5. Verify by reading back: `CLK_DIVISOR` is `007F0001` (or `(127 << 16)` with the old low half), `CONFIG_LOAD` bit 0 clear,
   `STATUS.BUSY` 0, `INT_STATUS` unchanged. Any mismatch: stop, show the words.
6. One TMP451 ID read as in T1 v2. Take the hardware microsecond timer (`TIMERUS_CNTR_1US`, read only) before the start and after
   `BUSY` clears, with the status poll as tight as T1 v2 allows. Record the elapsed time on ACK and on NOACK.
7. Show before and after words and the elapsed time. No restore write: the next hekate `hw_init` rewrites the divisor.

If the review prefers the state put back, W3 and W4 would be W1 and W2 with the old value after step 6; that adds two writes
after the bus has already been used.

## Failure and recovery

| Situation | Action |
| --- | --- |
| Any gate or idle check fails | Stop before W1. Nothing was written. |
| `CONFIG_LOAD` bit 0 stays set | Stop. Do not write the old value back. Hold the power button; hekate's next `hw_init` rewrites `CLK_DIVISOR` and runs a bus clear. |
| Readback differs from W1 | Stop, show it. |
| NOACK or timeout on the transfer | Stop. No retry, no bus clear, no controller reset (those are writes the constraints rule out). |
| Total time | The deadlines of the earlier runs stay provisional human deadlines (a 30 s transfer deadline, a 50 s total). They are not thermal values. |
| Screen or loader hang | Hold the power button. |

## Other devices on I2C1

I2C1 carries the BQ24193 charger, the MAX17050 fuel gauge and the BM92T36 as well as the TMP451 (hekate's drivers). A slower
divisor changes only the timing of transactions that start after W2. In the loader window no other master or driver is
known to use I2C1: BL31 does not (`ERISTA_BOOTCHAIN_CAR.md`), and the BPMP-FW build read here addresses I2C5 only
(`ERISTA_BPMPFW.md`, limits stated there). This is a reading of code that was not run, so a device that polls I2C1 by its own
means is not excluded. T1 v2 and T1-R already ran transactions at the faster, unmeasured speed with no recorded effect on
those devices. The run keeps the charger out and the battery at 100 % as T1 v2 did.

## Independence from T1 v2

- New entry, ident and package directory under their own names (`Switch` prefix, no existing name reused). T1 v2's
  `bl33.bin` (`23ac56e8...`), `bl31.bin`, `script.txt` and ini stay byte for byte as recorded in `ERISTA_T1.md`; a Bazel test
  compares their hashes.
- v3 shares only read-only helper code that already exists. It does not edit the v2 sources.
- The read-only gate reuses T1-R's register list and decode, which are separate from v2 as well.

## Unverified

- The TMP451 ACKs in shutdown (data sheet silent), and `INTERFACE_TIMING_0/1` can be read with the values the model
  assumes (Linux never reads them). Step 2 and the timer in step 6 are there to turn these from assumptions into data.
- The module clock is 102 MHz. The source is the Linux clock model applied to a measured word.
- `CONFIG_LOAD` bit 5 and bit 2 meanings are not documented in anything read.
- The writer of the changed sources is unknown (`ERISTA_HEKATE_BOOTPATH.md`). The test does not look for it.

## For the reviewer

Decide: divisor 127 or 255; `CONFIG_LOAD` value 1 or hekate's `0x25`; restore after the transfer or not; whether the gate
may continue when `INTERFACE_TIMING_0/1` read unexpected values. The implementation is not started.
