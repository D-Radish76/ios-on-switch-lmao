# T1-T result: first hardware run

Run 2026-11-26 on the Erista (`T210B01=0`, `SKU=0`, `BOOT=0`), hekate 6.5.5, from Nyx "More configs". The user reported it at
03:48 JST with a photograph of the loader screen. The words below are transcribed from it and the screen text agrees with the
loader's own output line for line (`t1t_measured_test.cpp`). The photograph is not stored in the repository. The record is
`boot/diag/erista-t1t-run1.txt`.

The screen showed `T1T COMPLETE CODE 0000`, `REGISTER READS 25`, `EL1 STATE PASS`, `BOOT STAYS 0` and the `RESET IN 15 S` line.
hekate came back by itself, and Nyx then showed 34.6 C (reported by the user at 04:00 JST; one reading, no load comparison). One observation per word. Nothing
here shows that a value is a reset value, is stable, or is safe.

## Measured words

| Word | I2C1 (`7000c000`) | I2C5 (`7000d000`) |
| --- | --- | --- |
| `CNFG` | `00002842` | `00000C00` |
| `STATUS` | `00000000` | `00000100` (BUSY) |
| `INT_STATUS` | `00000000` | `000000C2` |
| `CLK_DIVISOR` | `00050001` | `00190001` |
| `CONFIG_LOAD` | `00000004` | `00000000` |
| `TIMING_0` | `00000204` (TLOW 4, THIGH 2) | `00000204` |
| `TIMING_1` | `04070404` (TSU_STA 4, THD_STA 4, TSU_STO 7, TBUF 4) | `04070404` |

| CAR word | Value |
| --- | --- |
| `RST_DEVICES_L`, `CLK_OUT_ENB_L` | `049102CA`, `9802D131` (I2C1: clock on, reset clear) |
| `RST_DEVICES_H`, `CLK_OUT_ENB_H` | `84D07326`, `030180C1` (I2C5: clock on, reset clear) |
| `CLK_SOURCE_I2C1` to `I2C6` | `00000003`, `0`, `0`, `0`, `00000003`, `0` |
| `PLLP_BASE` | `48115408` (M 8, N 85, P 1) |

Reserved or unnamed bits of both timing words are 0 (`OTHER BITS 00000000`).

### Against earlier readings

- `RST_DEVICES_L`: T1 v2 was transcribed `049102CA`, T1-R `04D102CA`, T1-T reads `049102CA` from a photograph. Two of three
  agree. The T1-R word differs in bit 22 and may be a transcription slip. The T1-R photograph or video is the way to settle it.
  Bit 12 (I2C1) is clear either way, so nothing here depends on it.
- `CLK_SOURCE_I2C1` and `I2C5` `00000003`, `PLLP_BASE` `48115408`, I2C1 `CLK_DIVISOR` `00050001`, `CNFG` `00002842` and
  `CONFIG_LOAD` `00000004` repeat what T1-R and T1 v2 recorded.
- T1 v3's read-phase gates (source, PLLP fields, `BUSY`, `INT_STATUS`, divisor, `CONFIG_LOAD`, zero and all-ones timing words)
  accept every I2C1 word above. With the two timing words as pins the run reaches its first write
  (`t1v3_measured_test.cpp`). Without pins it stops at `061A` and writes nothing.

## I2C5: BUSY set and `INT_STATUS` 000000C2

What the words say, with the sources they come from:

- `STATUS` `00000100`: bit 8 is BUSY (hekate `I2C_STATUS_BUSY`), the NOACK field (bits 3:0) is 0.
- `INT_STATUS` `C2` is bits 1, 6 and 7. hekate names bit 6 `ALL_PACKETS_COMPLETE` and bit 7 `PACKET_COMPLETE`; Linux names bit 1
  `TX_FIFO_DATA_REQ` and bit 7 `PACKET_XFER_COMPLETE`. Bits 2 (arbitration lost) and 3 (no ack) are clear. So a packet transfer
  finished without an error flag, and nobody has cleared the flags.
- `CNFG` `00000C00` is `NEW_MASTER_FSM` (bit 11) and packet mode (bit 10: hekate `PACKET_MODE_GO`, Linux `PACKET_MODE_EN`),
  with no debounce count. Every `CNFG` write in hekate's driver includes `DEBOUNCE_CNT_4T` (`2 << 12`), and Linux's init writes a
  debounce count of 2 as well. I2C1's `00002842` fits the end of hekate's driver: 4T debounce, new FSM, `CMD1_READ`, a
  two-byte length field, packet mode off.
- `CLK_DIVISOR` `00190001` is the pair Linux uses for T210 (`clk_divisor_std_mode` and `fast_mode` `0x19`, `hs_mode` 1). hekate
  writes `00050001`.
- `CONFIG_LOAD` is 0, so no load is pending.

So I2C5 is not in the state hekate's driver leaves. Its words follow a Linux-style recipe: packet mode, divisor `0x19`, a
completed packet. Who wrote them is not known. The readings so far:

| Candidate | What is read against it |
| --- | --- |
| hekate's driver | `CNFG` and `CLK_DIVISOR` are not words it writes |
| BPMP firmware | its I2C5 transfer function writes `CMD_ADDR0`, `CMD_DATA1`, `CNFG`, `CONFIG_LOAD` and polls `STATUS` bit 8 (`ERISTA_BPMPFW.md`), but the scan finds no write of `CLK_DIVISOR` and it reaches I2C5 only through a function pointer |
| U-Boot (`bl33`) | the I2C driver is in the binary, the FDT enables I2C5 at 400 kHz, and the scan cannot see accesses through an FDT base address, so it is not excluded (`ERISTA_BOOTCHAIN_CAR.md`). Whether it probed I2C5 is not shown, and it does not explain I2C1's source word. |

BUSY set is one sample. It can mean a transfer in flight, a bus held after a transfer that sent no stop, or a flag nobody
cleared. T1-T read `STATUS` once. Nothing separates these.

For T1 v3: it touches only I2C1, whose `STATUS` and `INT_STATUS` are 0. The gate that stops on a non-zero `INT_STATUS` or BUSY
applies to I2C1 alone. If I2C5 were the bus under test it would stop at `0604`.

## I2C1 bus rate, recomputed

Model (Linux): `SCL = module clock / ((TLOW + THIGH + 2) x (divisor + 1))`. The timing words are now measured, so the
denominator no longer rests on Linux's programmed values: `(4 + 2 + 2) x 6 = 48`. The module clock is still a model:
`CLK_SOURCE_I2C1` `00000003` read as source `pll_p` (408 MHz, from `PLLP_BASE`'s row) divided by `3 + 1`. That is unverified, and
so is the integer reading of the divider.

| Case | Module clock | Divisor | SCL |
| --- | --- | --- | --- |
| hekate's intent (`C0000003`, clk_m 19.2 MHz / 4) | 4.8 MHz | 5 | 100 kHz |
| I2C1 as measured | 102 MHz | 5 | 2.125 MHz |
| I2C1 under T1 v3 (`00FF0001`) | 102 MHz | 255 | 49.8 kHz |
| I2C5 as measured | 102 MHz | 25 | 490 kHz |

- hekate's 100 kHz comment is exact only with `(TLOW + THIGH + 2) = 8`, so `TIMING_0` `0204` agrees with the assumption hekate's
  numbers were built on. It does not prove `0204` is the hardware's own reset value.
- T1 v2 therefore ran I2C1 at about 21 times the rate hekate meant, 2.1 MHz in the model and about five times the 400 kHz
  fast-mode rate. That fits T1 v2's lack of an ACK. It remains a hypothesis until T1 v3 gets an answer or not.
- The slowdown of T1 v3 against the present state is `256 / 6` = 42.7 whatever the module clock is, because source and timing words
  stay as they are. The absolute 49.8 kHz needs the 102 MHz model. If the source field 0 meant a 408 MHz clock with no divider,
  the same arithmetic gives 199 kHz. A divisor near `0x7F` would give about 100 kHz in the model; `0xFF` stays as approved.
- The `TIMING_1` fields (setup and hold values 4, 4, 7, 4) have no documented unit in anything read. Whether they count module
  clock cycles or divided cycles decides whether the divisor scales them. No setup or hold time is claimed.

## Pin values for T1 v3

Candidates, from this run only:

| Pin | Value | Basis |
| --- | --- | --- |
| `timing_0_pin` | `00000204` | read on I2C1 and I2C5; equals the value Linux programs for T210; hekate's 100 kHz arithmetic uses it |
| `timing_1_pin` | `04070404` | read on I2C1 and I2C5; Linux leaves this register at the chip default on T210, so nothing programs it there |

How they would act: T1 v3 compares each word to its pin before the first write and stops at `0607` on any difference, so a
pin is a second read of a known value before anything is written, not an assumption that the value is right. The run has no way to
test that the value is acceptable for the bus, only that it is the same as this run's.

What the pins would not tell:

- one observation, taken from the same launch path (Nyx "More configs") T1 v3 will use;
- equal words on two controllers could come from a shared default or from one writer touching both. This run cannot separate
  them;
- no reset value, reserved-bit mask or unit is documented for either register.

The change needs approval: `t1v3_access.c` holds the pins and the T1 v3 image hash changes with it (the other six hashes do
not). A second T1-T run (read only) would give a second observation if one is wanted before T1 v3. T1 v3's own read phase would
give the same observation with no write if the pins did not match.

## CONFIG_LOAD, what is open

| Item | State |
| --- | --- |
| Bit 0 (master load) | self-clears; hekate's `0x25` readback shows it clear. Not in doubt. |
| Bit 2 (hekate `TIMEOUT_CONFIG_LOAD`) | I2C1 reads 1 (written by hekate's init, untouched since); **I2C5 reads 0**. So bit 2 can read 0 after other activity. Open: is it a plain writable bit that a write without it clears (H1), or does a hardware event clear it (H2)? |
| What `0x01` does to bit 2 | unknown. T1 v3 writes `0x01` once and reads the word back. H1 predicts `0`, H2 predicts `4`. Its gate accepts both and stops on anything else, so the run itself discriminates. |
| Bit 5 | written as 1 by hekate, reads 0 on both. Self-clearing or not stored; unnamed in every source. |
| Bit 1 (FreeBSD `SLV_CONFIG_LOAD`) | never read set |
| Reset value of the register | not documented in any source read |
| The transfer writes `0x25` | the same word hekate writes at init. T1 v3 requires the readback to be exactly `00000004` afterwards. Under H1 the `0x25` sets bit 2 again; under H2 it never cleared. Both end at 4. |

## Remaining for the T1 v3 hardware run

Nothing below is done, and none of it starts a run. T1 v3 stays on hold.

1. Decision on the pins (`00000204`, `04070404`), with the limits above. Approval gates the `t1v3_access.c` change.
2. Decision on repeating T1-T once (read only) before T1 v3, or accepting one observation.
3. `TIMING_1` unit and whether the divisor scales it: unknown. A decision to proceed with this open is a review call.
4. Module clock: 102 MHz is a model. A measurement would use the CAR clock-frequency measurement registers, which are written,
   so it is not a read-only change and needs its own review.
5. I2C5's activity (BUSY, pending flags, a Linux-style configuration) and the writer of I2C1's and I2C5's source words stay
   unidentified. T1 v3's idle check covers I2C1 for its own window; nothing watches the CAR during the run.
6. The `CONFIG_LOAD` bit 2 behaviour is left to the run itself (above). The gate accepts `0` and `4` after the apply.
7. `ERISTA_T1V3_RUN.md` does not exist yet: Mac commands (`--config=ios17`), hashes, the expected screen with the pinned
   words, deadlines, stop codes, recovery. After the first run T1 v3 leaves `00FF0001` in I2C1 until a reset; hekate's
   `i2c_init` rewrites the divisor at its start (source, not observed).
8. Done: the loader returned to hekate by itself and Nyx showed 34.6 C afterwards. One reading, not a limit.
9. The T1-T hardware result is not a safety result. Every value stays a candidate.

Sources: hekate `bdk/soc/i2c.c` v6.5.5; Linux `drivers/i2c/busses/i2c-tegra.c` v6.12 (fetched 2026-11-26); `ERISTA_BPMPFW.md`,
`ERISTA_BOOTCHAIN_CAR.md`, `ERISTA_T1V3_DESIGN.md`, `ERISTA_T1T_DESIGN.md`.
