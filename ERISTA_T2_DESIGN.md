# T2: one TMP451 temperature conversion

Design only. Nothing here is implemented, built or run, and no write below is approved. T2 stays on hold with T3, T4, D1 and T1-H, and
`boot` stays 0. It builds on `ERISTA_T1V3_RESULT.md` and replaces the T2 paragraph of `ERISTA_THERMAL_DESIGN.md`.

Sources: TI TMP451 data sheet (fetched 2026-11-26), hekate 6.5.5 `bdk/soc/i2c.c` and `bdk/thermal/tmp451.c`, Linux `lm90.c` v6.12, and
the T1 v3 run.

## What T2 does

T2 starts one conversion on a sensor that is in shutdown, waits for it to finish, and reads the local (PCB) and remote (SoC diode)
temperatures. It starts no fan, writes no sensor setting and changes no clock. It leaves the sensor in the shutdown state it found.

It runs on the I2C1 setup T1 v3 proved: divisor `00FF0001` (about 42 to 51 kHz), the same gates, the same five I2C1 registers. Only that
rate has been shown to work, so T2 does not try another.

## TMP451 facts used

| Item | Data sheet |
| --- | --- |
| Address | 0x4C. Write `0x98`, read `0x99` |
| Read pointers | local high `00`, remote high `01`, status `02`, configuration `03`, remote low `10`, local low `15`, manufacturer ID `FE` (`55`) |
| Write pointers | configuration `09`, conversion rate `0A`, one-shot start `0F` (write-only) |
| One-shot | "When the device is in shutdown mode (SD = 1 in the configuration register), a single conversion is started by writing any value to the one-shot start register, pointer address 0Fh." The device shuts down again after the conversion |
| Conversion time | 31 ms minimum, 34 ms typical, local plus remote, one-shot. **No maximum is published** |
| Status | bit 7 BUSY ("ADC is converting"), 6 LHIGH, 5 LLOW, 4 RHIGH, 3 RLOW, 2 OPEN, 1 RTHRM, 0 LTHRM. Flags clear on a status read when the condition is gone. The data sheet does not say that BUSY is set during a one-shot; that is an inference |
| Data | 12 bits, 0.0625 C. High byte 1 C per step, upper nibble of the low byte holds the fraction. Standard range 0 to 127 C: below 0 reads 0, above 127 reads 127. Extended range is -64 to 191 C with a +64 offset |
| Fault | open remote diode sets OPEN. A short reads -64 C (in the extended range; the standard range clamps at 0, so a short is not told apart from 0 C there) |
| Read order | "read the high byte of the temperature result first", then the low byte |
| Bus | fast mode 1 to 400 kHz, high-speed 1 to 2.5 MHz. The bus timeout (SMBTO) is optional and off by default; when on it resets the interface after 25 ms (typical) of SCL or SDA held low |

hekate's driver uses the same transfers on this sensor: `i2c_send_byte` is one write transaction of two bytes (`CNFG` = `0x2802`
plus the go bit), and `tmp451_init` writes configuration `0x80` and the offsets through it; `tmp451_end` writes `0xC0` (shutdown).
Linux's `lm90.c` never triggers a one-shot (it runs the part free), so there is no kernel precedent for the one-shot form. The data sheet does not say whether a
send-byte (pointer only) or a write-byte (pointer and a value) is wanted. T2 uses the write-byte form, since "any value" is written,
and the form hekate already uses on this part. Confirm against the data sheet's timing diagrams before code.

## Entry state

`tmp451_end` leaves the sensor in shutdown with configuration `0xC0` (bit 7 alert mask, bit 6 SD) before the entry runs. Nobody has read
the configuration. T2 reads it first and does not assume.

## Sequence

All transfers are T1 v3's normal-mode transactions, each one attempt, no retry.

1. T1 v3's prelude: the gates (clock, reset, source `00000003`, `PLLP_BASE` fields, BUSY, `INT_STATUS`, divisor `00050001`,
   `CONFIG_LOAD` `00000004`, timing pins, three idle samples), then write 1 and 2 (divisor, `CONFIG_LOAD` `0x01`).
2. Sensor ID `FE`. Not `55`: stop, no sensor write.
3. Configuration `03`. Anything but `0xC0`: stop, no sensor write, word shown. (`0xC0` is hekate's `tmp451_end` word. A different word is a
   finding: the device is not in the state this design assumes.)
4. Offsets `11` and `12`, status `02` (expect BUSY clear: a BUSY here means a conversion in flight, stop), then the last conversion:
   remote high `01`, remote low `10`, local high `00`, local low `15`. These are the sensor's old values and are shown as such.
5. One-shot: write `0F` with value `00`. This is the only sensor write.
6. Completion. After the write transaction's own BUSY clears, the first status read is at about 2 ms (record BUSY). No completion is
   accepted before 31 ms have passed since the write. From 34 ms on, read status at 5 ms steps until BUSY is clear. Bound: 100 ms from the
   write, then stop. The bound is a design choice (about three times typical), not a TI figure.
7. Configuration again: SD must still be set. A cleared SD is shown, nothing restores it.
8. New values, high byte first: remote high, remote low, remote high again (a change between the two high reads means the pair is not
   from one conversion and is shown as `INCONSISTENT`; in shutdown it should not change), local high, local low.
9. The screen shows old and new, the status words, the conversion time from the write, the OPEN flag and the decoded temperatures.

## Abnormal conditions

| Condition | Result |
| --- | --- |
| Any T1 v3 gate, `CONFIG_LOAD`, BUSY, NOACK or timeout stop | the same stops and rules as T1 v3 (`ERISTA_T1V3_RUN.md` 5, 7, 10), stop code range `07xx` |
| ID not `55`, configuration not `0xC0`, status BUSY before the write | stop before the one-shot write |
| NOACK on the one-shot write | stop, no retry. Whether the sensor ran is unknown and shown as such |
| Status BUSY never seen | recorded, not a failure by itself (the BUSY semantics for one-shot are unproven) |
| BUSY still set at 100 ms | stop `CONVERSION TIMEOUT`, no later reads, no restore |
| OPEN set | the remote value is marked invalid and not used. It does not stop the run |
| Remote 0 C or 127 C | shown as `AT RANGE EDGE`, not as a temperature: it may be a short, an open, or below or above the range |
| SD cleared after | shown as `NOT IN SHUTDOWN`, no write |
| Two high reads differ | `INCONSISTENT` |

Display highlight, not a limit: a result of 70 C or more is drawn red. That figure is provisional and is not an official limit of any
part.

No restore after a stop. The next Nyx start runs `tmp451_init`, which writes the sensor's configuration, offsets and rate.

## All writes

Every device write goes through the T1 v3 allowlist, extended only in values. Registers are I2C1 at `7000c000`. No CAR, I2C5,
`TIMING_0/1`, pinmux, GPIO, PWM or PMC write.

| Register | Values | Used for |
| --- | --- | --- |
| `CLK_DIVISOR` (`+6c`) | `00FF0001`, once | the rate T1 v3 proved |
| `CONFIG_LOAD` (`+8c`) | `01` once; `25` before every transaction | apply the divisor; load the transaction |
| `CMD_ADDR0` (`+04`) | `98`, `99` | write and read address |
| `CMD_DATA1` (`+0c`) | `FE 03 11 12 02 01 10 00 15` pointers, `0F` for the one-shot | register pointer; for the one-shot, pointer `0F` and value `00` in one word |
| `CNFG` (`+00`) | `2800`, `2840` no go; `2A00`, `2A40` go; `2802` no go and `2A02` go for the one-shot | one-byte write, one-byte read, two-byte write |

New against T1 v3: the pointers `03 11 12 02 01 10 00 15` in `CMD_DATA1`, and `2802` and `2A02` in `CNFG`. `CMD_DATA1` `0F` is also the
pointer-only word; `CNFG`'s length field (`2802`) tells the controller to send the second byte.

### Sensor register operations

| Op | Direction | Register | Value |
| --- | --- | --- | --- |
| ID | read | `FE` | expect `55` |
| Configuration, before and after | read | `03` | expect `C0` |
| Offsets | read | `11`, `12` | shown |
| Status | read | `02` | BUSY, OPEN, flags |
| Last conversion | read | `01`, `10`, `00`, `15` | shown |
| **One-shot** | **write** | **`0F`** | **`00`** |
| Status polls | read | `02` | until BUSY clear |
| New conversion | read | `01`, `10`, `01`, `00`, `15` | shown |

One sensor write in total. Each sensor read is a pointer write plus a read: 9 MMIO writes. The one-shot is 5. The prelude is 2. With
the 1 or 2 status reads counted, a run is about 150 to 190 MMIO writes in about 55 ms of bus time.

## Time and heat

At 433 us per transaction the bus time is about 55 ms and the conversion wait about 34 ms of that. The run, not the conversion, is short.
The loader still reads no temperature stop of its own: the result is shown for a human, the 15 s countdown and the 50 s deadline apply,
and the Nyx value before and after is reported as for T1 v3. T2 does nothing that cools or heats the console.

## Open items (N7-D)

Settled from the data sheet (TI SBOS686A, sections 7.4.2 and 7.6, read 2026-11-26):

- Write-byte, as designed. Section 7.4.2: "a single conversion is started by writing any value to the one-shot start register,
  pointer address 0Fh" and "the value of the data sent in the write command is irrelevant and is not stored". The text says
  "writing" and names a data byte, so the write-byte form is the documented one. It never calls a pointer-only transfer a trigger.
- The remote offset registers are read and written at the same pointers: high `11`, low `12`.
- BUSY (status bit 7) is "1 = ADC is converting". The data sheet does not say it is set during a one-shot. Still an inference.

Still open:

- The conversion time's upper bound. 31 ms minimum and 34 ms typical come from the electrical characteristics; the pages read here
  gave no maximum. The 100 ms bound stays a design choice.
- Whether BUSY is set during a one-shot (needs a run, or the timing diagram).
- Whether the sensor ACKs reads other than the ID at the changed divisor while in shutdown. T1 v3 shows the ID read only.
- Who may run T2, and the rate. T2 does not use 100 kHz because 100 kHz has not run.
- The polling in the loader: the bus is read by counter, as every loader stage is. A kernel driver would use the ALERT line or a timer.

## Prepared on the host (N7-D)

`boot/diag/t2/t2.c` is the sensor sequence over a bus of single register transfers (ID, configuration, offsets, status, old
sample, the one write, the wait, configuration again, new sample with the repeated high read). `t2_test` runs it against a model
of the part: one write only, none before the entry checks pass, no completion before 31 ms, the 100 ms bound, a failed read at
every position, an inconsistent pair, a cleared shutdown bit. The image section below links it.

## Bus layer (N8)

`boot/diag/loader/t2_bus.c` turns T1 v3's register table into single sensor transfers: a read is a pointer write then a read,
the one-shot is one two-byte write (`CMD_DATA1` `0F`, `CNFG` `2802`, go `2A02`). `switch_t2_may_write` is T1 v3's allowlist with
the pointers and the two `CNFG` values added; `CLK_DIVISOR` is refused outright, since the prelude owns that one write.
`t2_bus_test` runs the sequence over a model of I2C1 with the part behind it: one sensor write, no register outside `CNFG`,
`CMD_ADDR0`, `CMD_DATA1` and `CONFIG_LOAD` written, the allowlist exhaustive over 16 bits, nothing written after a stop, NOACK,
no BUSY and a sticky load each stopping the transfer. Stop codes `0730` to `0736`.

## Image (N8, offline)

`//boot:diag_t2` is an independent diagnostic image. It copies T1 v3's prelude (CAR checks, controller checks, idle samples, the
one divisor write) into `t2_prelude.c` instead of exposing it, so T1 v3's compiled bytes and hash do not change. T2 has its own
register table and allowlist (`t2_regs.c`), write gate (`t2_gate.c`), bus layer, screen lines (`t2_report.c`) and entry
(`t2_image.c`). Nothing in it names a T1 v3 symbol.

Stop codes: `0701` to `071b` controller prelude, `0720` to `0725` sequence, `0730` to `0736` bus. The flat image hash is the
`diag_t2` line of `erista-diag-image-hashes.txt` (`7f5a91a4...`, Linux clang 18.1.3); the seven earlier lines are unchanged.
`diag_loader_inspect.py` checks it as it checks the others: raw register accesses sit in one instruction each, only the gate
calls them, and each of the 13 register addresses appears once in the linked image.

Checked offline: `t2_bus_test` (host), `diag_t2_test` (image inspection, link, allowlist, entry, hashes). Not run here: Bazel,
and the Mac build of `//boot:diag_t2`. The audit tool has no `--t2` mode yet. No hardware run is approved; T2 stays HOLD.

## Host tests, when approved

A model of the sensor: shutdown, ID, the one-shot with a configurable conversion time, status BUSY, OPEN, range edges. Tests: normal run;
no sensor write unless ID and configuration match; exactly one sensor write; completion not accepted before 31 ms; the 100 ms bound; OPEN
and the range-edge handling; the allowlist pairs of value and register; zero writes after any stop; the T1 v3 stop rules.
