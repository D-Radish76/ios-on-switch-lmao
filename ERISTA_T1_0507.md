# T1 v2 first run: stop 0507 (NO ACK from 0x4C)

Run 2026-11-25 on an Erista from Nyx "More configs", commit `86346c224`. Values: `boot/diag/erista-t1-run.log`
(transcribed by the reviewer from the photograph), checked by `tools/package/test/t1_run_test.py` and by one case in
`boot/diag/loader/test/t1_test.cpp`. Investigation and design review only: nothing was changed in the loader, the address,
the clocks, the bus or the sensor. `boot` stays 0.

## What the run showed

- Passed on hardware: the loader started, the counter check passed, the CAR and I2C1 reads returned, the first transfer
  failed, T1 stopped with code 0507 and no retry, drew its report, waited 15 s and reset. hekate came back by itself with no
  button hold (reported by the operator). That is the first hardware pass of the abnormal stop path and the automatic return.
- Not shown: any TMP451 communication, any temperature, and whether Nyx's temperature control works afterwards. The Nyx check
  in `ERISTA_T1_RUN.md` section 6 has not been reported; the return to hekate does not replace it.

## The finding

hekate programs the I2C1 clock source register as `(6 << 29) | 3` = `0xC0000003` (`clock_enable`, `_clock_i2c[0]`, source 6,
divisor field 3). The loader read `SRC_I2C1 = 0x00000003`: the divisor is hekate's, the source field is 0.

| Register | Read | hekate's value | Match |
| --- | --- | --- | --- |
| `SRC_I2C1` | `00000003` | `C0000003` | no, source field 0 instead of 6 |
| `SRC_PWM` | `C0000004` | `C0000004` (`clock_enable_pwm`) | yes |
| `DIV` (`CLK_DIVISOR`) | `00050001` | `00050001` (`i2c_init`) | yes |
| Pads SCL, SDA | `00000040` | input enable only (`pinmux_config_i2c`) | yes |

Nothing else in the hekate tree writes `CLK_SOURCE_I2C1`. The pinned `bl31.bin` has no CAR address literal. The pinned
`bl33.bin` has the CAR base once (its clock driver) and the Tegra I2C driver, but its control FDT disables I2C1, so a probe of
I2C1 by U-Boot is not expected; this was not run or read as code. So something after hekate's `hw_init` changed the source
field and left the divisor alone. Which one is not known.

What this would mean for the bus, if source 0 is PLLP_OUT0 at 408 MHz (an assumption: from Linux's mux order, not from a TRM
read here, and the PLLP rate at that moment is not measured). hekate's own comment gives the clock as
`Fin / (TLOW + THIGH + 2) / FM_DIV` with TLOW 4, THIGH 2 and `FM_DIV` = field 31:16 + 1 = 6, and the I2C source divider as
`Fin / (field + 1)`. With 19.2 MHz that is 4.8 MHz and 100 kHz, hekate's figure. With 408 MHz it is 102 MHz and about
2.1 MHz, which is outside what a fast-mode part is specified for. This would explain an address phase with no ACK. It is the
leading hypothesis, not a conclusion: the timing registers (`TLOW`, `THIGH`) were not read, PLLP was not read, and the TMP451's
tolerance of that rate (the datasheet has a high-speed section; its limits were not in the pages I could fetch) is not shown.
The unit tests assert the decoded facts and the arithmetic, not that this is the cause.

## Sequence comparison with hekate

`_i2c_send_normal` and `_i2c_recv_normal` against T1's `transaction`:

| Step | hekate | T1 | Same |
| --- | --- | --- | --- |
| Address | `dev << 1` plus 0 (write) or 1 (read) = 0x98, 0x99 | 0x98, 0x99 | yes |
| Direction | `CMD1_READ` = bit 6 | 0x40 in `CNFG` | yes |
| Data | `CMD_DATA1` = the register pointer, written before `CNFG`, writes only | same | yes |
| `CNFG` before load | `((size-1) << 1) + 0x2000 + 0x800 + dir` = 0x2800 or 0x2840 for one byte | 0x2800, 0x2840 | yes |
| `CONFIG_LOAD` | writes 0x25, waits at most 20 us for bit 0 | writes 0x25, waits at most 1 ms for bit 0 | value yes, wait longer |
| Go | `CNFG` with bit 9 set | 0x2A00, 0x2A40 | yes |
| After | wait `BUSY`, then any `STATUS` bit 3:0 is failure | same, after a 250 us guard | yes |
| Order | pointer write, then read, no repeated start, no FIFO use | same | yes |

No difference in the register sequence was found. The first transfer is the write of pointer 0xFE; it failed, so nothing was
read. This does not prove the sequence is right on this silicon, only that it is hekate's.

## STATUS AFTER 00000001

T1 reads `I2C_STATUS` (0x1C) after the last transfer attempt. `00000001`: `BUSY` (bit 8) clear, the NOACK field (bits 3:0)
equals 1. hekate and T1 treat any of those four bits as no ACK; what each bit means is in the TRM, which was not found here.
That bit 0 is the address phase is a guess. The word is read after the failed transfer and after `STATUS` showed `BUSY` clear,
so it is the state at the end, not a count. `INT_STATUS` after the transfer (where `NO_ACK`, bit 3, would show) is read and
stored in the probe record but is not on the screen, so it is unknown. `STATUS` before was 0, so the bit was set by this
transfer.

## TMP451 in shutdown

hekate left the sensor in shutdown (`tmp451_end`, config 0xC0) before this run. The TMP451-Q1 datasheet text I could read says
shutdown stops "all device circuitry other than the serial interface"; it does not say the part still acknowledges, and the
sections on bus speed and the input filter were not in the pages fetched. So: shutdown is not shown to be the cause, and not
shown to be harmless. Nyx, on return, restarts the sensor (`tmp451_init` writes config 0x80).

## I2C1 and pinmux through BL31 and U-Boot

What is known is the entry state in the table above; what was it before U-Boot is not known, because nothing read I2C1 or the
CAR before the loader. Pads equal hekate's write. `CNFG` is `0x2842`: debounce 2, new master FSM, read, length field 1 (two
bytes), no go bit; the shape of a finished two-byte normal-mode read, which fits Nyx or hekate reading a 16-bit fuel gauge
register. `CONFIG_LOAD` reads 4: bit 0 clear, the timeout-load bit set; that is what hekate's own 0x25 write leaves behind
(bits 0 and 5 clear themselves, bit 2 does not), which supports hekate having been the last user of the controller.

## PWM clock on

Measured: `ENB_L` bit 17 set, `RST_L` bit 17 clear, `SRC_PWM = C0000004`, the value hekate's `clock_enable_pwm` writes. The
design (`ERISTA_THERMAL_DESIGN.md`) expected the clock on at the loader because the display backlight leaves it on, which this
agrees with. The only writer of that exact value in the hekate tree is `clock_enable_pwm`, which the fan code calls only on an
Aula, so what wrote it on this Erista was not found. T3 must read the clock state and not assume it. T1 touched neither the PWM
block nor its clock. The fan state itself was not read.

## What would tell the hypotheses apart (proposals, not made)

Read only, each needing review before any run:

1. Show `INT_STATUS` after the transfer on the screen (already read).
2. Read the I2C1 interface timing register and PLLP's base register, and show the decoded SCL estimate.
3. Read `CLK_SOURCE_I2C1` in the U-Boot script, before `bootm`, with `md.l`, to learn whether the field is 0 already before the
   loader runs. It cannot say whether hekate or U-Boot's init did it.
4. A check in T1 that stops before any transfer when the source field is not hekate's: this run would have ended at that
   check with no bus access. It treats the symptom, so only after the cause is known.
5. Not proposed: writing the source field, changing the divisor or address, a bus clear, reading another I2C1 device to see
   whether the whole bus fails (a new device access that needs its own review).

Until the cause is known: no address change, clock change, bus clear or sensor setting; T1 is not re-run; T2 to T4 and D1
stay on hold.
