# T1 v3: options for the I2C1 clock (comparison only)

Finding the writer of the changed clock sources and getting the TMP451 to answer are separate problems. This note is about
the second. Nothing is implemented, no register is written, T1 v3 and everything after T1-R stay on hold, `boot` stays 0.
Sources: Linux (`clk-tegra-periph.c`, `clk-divider.c`, `clk-tegra210.c`, `i2c-tegra.c`), hekate 6.5.5 source, TI's
TMP451 data sheet (SBOS686A), the T1 v2 and T1-R runs. The Tegra X1 TRM was not available.

## Facts

Confirmed in Linux source (not the TRM):

- The six I2C source registers use the macro `I2C(...)`: mux at bit 30, 2 bits wide (`mux_pllp_clkm`: `pll_p` index 0,
  `clk_m` index 3), 16 bit divider, no fractional bits (`frac_width` 0), `TEGRA_DIVIDER_ROUND_UP`. The rate is
  `parent / (field + 1)`. This settles the earlier inference: `00000003` is `pll_p / 4`, `C0000003` is `clk_m / 4`.
- The controller divisor register `I2C_CLK_DIVISOR` (offset 0x6c) holds the standard and fast divisor in bits 31:16 and the
  high-speed one in bits 15:0. Linux's T210 values: `clk_divisor_hs_mode` 1, `clk_divisor_std_mode` and `_fast_mode` 0x19.
  The measured `00050001` is hekate's `(5 << 16) | 1`.
- Linux computes the module rate it asks for as `bus rate x (TLOW + THIGH + 2) x (divisor + 1)`, T210 `TLOW` 4, `THIGH` 2,
  and writes `INTERFACE_TIMING_0` (and `_1` when non-zero); it never reads them. T210 has `enable_hs_mode_support = false`.
- hekate never writes `INTERFACE_TIMING_0/1` (no match in `i2c.c`). Its 100 kHz and 400 kHz comments therefore assume the
  reset values. Unverified.
- A divisor write takes effect after the config load strobe (`I2C_CONFIG_LOAD`, `_i2c_load_cfg_wait`).
- TMP451 data sheet 7.4.1: shutdown mode shuts down "all device circuitry other than the serial interface". It does not
  state that the part ACKs in shutdown; the electrical table lists shutdown current with the bus active at 400 kHz and
  2.5 MHz. So an ACK in shutdown is expected and still an inference. Fast mode 1 to 400 kHz (tLOW min 1300 ns, tHIGH min
  600 ns); high-speed to 2.5 MHz only after a master code `0000 1xxx` that the part does not acknowledge (tLOW 260 ns, tHIGH
  60 ns). The SMBus timeout is off by default.
- On the Switch, I2C1 also carries the BQ24193 charger, the MAX17050 fuel gauge and the BM92T36 (hekate's drivers), so a
  change to the bus speed reaches every device on it.

Derived, with the unverified timing at its assumed values:

| Source word | Module clock | SCL, divisor 5 |
| --- | --- | --- |
| `C0000003` (hekate) | 19.2 MHz / 4 = 4.8 MHz | 100 kHz |
| `00000003` (measured) | 408 MHz / 4 = 102 MHz | 2.125 MHz |

At 2.125 MHz the period is 470 ns. Fast mode needs tLOW plus tHIGH of at least 1900 ns, and the high-speed timings do not
apply without a master code, which the T1 v2 sequence (equal to hekate's `_i2c_send_normal`) does not send and the
T210 controller is not enabled for in Linux. A part in fast mode being driven that fast is out of specification, which fits the
NOACK of T1 v2 and does not prove it. A bus fault and a part that does not answer in shutdown are not excluded.

## Does a CAR change reach only I2C1?

`CLK_SOURCE_I2C1` (0x124) is the controller's own register, so writing it affects I2C1 only. The parents (`pll_p`,
`clk_m`) are shared by many peripherals and are not touched by a source or divider write. The enable and reset bits of
I2C1 sit in `CLK_OUT_ENB_L` and `RST_DEVICES_L` bit 12, words shared with other peripherals (a read-modify-write of those
would touch them; hekate uses the `_SET` and `_CLR` registers for that reason). I2C5's hekate comment about sharing with
CL-DVFS does not apply to I2C1. A change after I2C1 has gone quiet affects whoever still uses I2C1 later: the loader runs
after BL31, which does not use it (`ERISTA_BOOTCHAIN_CAR.md`); BPMP-FW's only I2C user is I2C5.

## Options

| | What changes | Writes | Effect on the bus | Undo | Evidence |
| --- | --- | --- | --- | --- | --- |
| 0. Nothing | none | none | NOACK stays | none | T1 v2 |
| 1. Raise the controller divisor | `I2C_CLK_DIVISOR` field 31:16 from 5 to 127, then config load | 2 stores in the I2C1 block, no CAR | SCL = 102 MHz / (8 x 128) = about 99.6 kHz (31 gives about 398 kHz, too close to the limit) | write `00050001` and load again; hekate's `i2c_init` writes it at every boot | Linux formula, assumed TLOW 4 and THIGH 2, field width 16 bits |
| 2. Put back hekate's source word | `CLK_SOURCE_I2C1` to `C0000003` | 1 store to CAR (a measured register), with hekate's reset, gate, write, enable, release sequence if the controller is reset | 4.8 MHz, SCL 100 kHz | write `00000003`; hekate rewrites it at boot | hekate's own table |
| 3. Keep `pll_p`, raise the CAR divider | `CLK_SOURCE_I2C1` field 3 to 84 | 1 store to CAR | 408 / 85 = 4.8 MHz, about 100 kHz | as 2 | Linux integer divider |
| 4. Hs mode | master code, Hs timing | many | up to 2.5 MHz | | The controller's HS support is off in Linux and untested here. Not suggested. |
| 5. Find and stop the cause | | | | | The writer is unidentified (`ERISTA_BOOTCHAIN_CAR.md`), so not a v3 task. |

Option 1 leaves the clock tree and every CAR register as they are and changes only a speed that is in the controller; it
is the closest to "keep the current setting and adjust the speed". Its limits: it needs the timing registers at the
assumed reset values (T1-R never read `INTERFACE_TIMING_0/1`, and a read has no known side effect but no proof either), and
it is a store to the I2C1 block, which T1 v2 already used for transfers. Options 2 and 3 write a register that T1-R and
the boot chain measure and are the closer to hekate's own state. Option 2 is the plain restore and carries the
reset question below.

## Before any of them

- Read first, and only read: `INTERFACE_TIMING_0/1` (0x7000c094, 0x7000c098) and `I2C_CLK_DIVISOR` with the I2C1 words
  T1-R reads, with I2C1 guarded on its enable and reset bits as in T1-R. Without the timing words the 2.1 MHz figure and the
  choice of divisor stay assumptions.
- The sequence must not start with a transfer on a bus that may be wedged. Hekate's `i2c_init` runs a bus clear (a write
  to `I2C_BUS_CLEAR_CONFIG`); that is an I2C write too.
- Reset: hekate asserts reset and gates the clock around a source write. Whether a source write with the clock running
  and the controller idle is safe is not shown in what was read (Linux's `clk_set_rate` does it without a reset on an idle
  controller). The reset and gate values T1-R read (bit 12 of both words) show I2C1 enabled and out of reset.

## Failure and recovery

| Failure | Recovery |
| --- | --- |
| NOACK or timeout | Stop, write nothing more, leave the divisor or source as it is, show the status words. |
| Bus held low | I2C bus clear (a write), then a controller reset through `RST_DEVICES_L` set and clear for bit 12 (also CAR writes). Neither is allowed by the current constraints. |
| Controller or device hung | Hold the power button. The next boot reruns hekate's `hw_init` (`clock_enable_i2c`, `i2c_init`), which rewrites the clock source, the divisor and the bus clear. |
| TMP451 left running or in shutdown | Nyx restarts it (`tmp451_init`) and updates the temperature every few seconds. |

A change made in the loader does not outlive the next hekate `hw_init`. It also has no effect on the next boot if the
console is reset through BL31's reboot-to-payload, because that path runs hekate again.

## Reading the result

If a changed speed makes the TMP451 answer, the clock hypothesis is supported and the cause stays unknown. If it does
not, the clock was not the only problem. The temperature read is for the console's state with the fan off and nothing
else; the 50 s deadline is a provisional human deadline, not a thermal value.

## Thermal note

Correction to `ERISTA_T1H_DESIGN.md`: the Nyx Payloads path and the l4t launch differ by hekate's second `hw_init`, its file
and configuration work, and the BPMP-FW, BL31 and U-Boot stages, so their thermal conditions are not shown to be the same.
The fan is off from Nyx's `hw_deinit` to the end of every one of these runs.
