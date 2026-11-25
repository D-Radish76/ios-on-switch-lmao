# T1 second review: findings and proposed fixes

2026-11-25, after the Mac build began. Documentation only: no T1 source, binary, test expectation of the image or hash
changed. One test (`TransferOrderIsHekatesFromTheFirstWriteToTheLastRead`) was added to `t1_test.cpp`; it passes against the
current code. T1 stays unrun.

Findings 1 to 3 were then approved and applied (T1 v2, `ERISTA_T1.md` "Revisions"); finding 4 was kept as is; finding 5 is
unchanged by decision. The text below is the review as written.

## Findings

| # | Finding | Effect | Proposed fix |
| --- | --- | --- | --- |
| 1 | **T1 reads TMP451 configuration and rate through their write addresses.** TI Table 3: configuration is read at 0x03 and written at 0x09; conversion rate is read at 0x04 and written at 0x0A. T1 sets the pointer to 0x09 and 0x0A and reads. The datasheet does not say what such a read returns. hekate only ever writes 0x09 and 0x0A. | The two bytes shown as `CONFIG(09)` and `RATE(0A)` have no defined meaning. No harm to the sensor (a pointer-only write changes no data), but the screen would mislead. | Pointer values 0x03 and 0x04 in `switch_t1_tmp451_register`, the `CMD_DATA1` allowlist, the labels (`CONFIG(03)`, `RATE(04)`) and the tests. Status 0x02 and the write addresses stay out of the list. |
| 2 | **The 100 ms and 1 ms bounds depend on a counter nobody checked first.** `io_now_us` reads `CNTPCT_EL0` (the ARM generic counter, 19.2 MHz, `isb; mrs; x * 5 / 96` in the disassembly). The entry check requires `CNTFRQ` = 19.2 MHz. Whether the count advances is tested only in `exit.c`, after T1 has run. A frozen counter makes `now - start` stay 0 and the `BUSY` loop never ends. | Not seen on this console: the D0 and D0-R runs passed the same check at exit and then reset, so the counter advanced there. It is still an unbounded loop. | Before the first I2C1 access run `counter_advances` (the check `exit.c` has) and stop with a new code if it fails. Also bound every poll loop by an iteration count that is far above what 100 ms allows. Both fixes touch only `t1.c` / `t1_access.c`. |
| 3 | **The first `BUSY` poll can run before the transfer has started.** Same as hekate's driver: `go` is written and `STATUS` is read at once. If `BUSY` is not yet set, the loop exits and the NOACK and data reads happen early. A read would then return the pointer byte still in `CMD_DATA1`. | The ID check catches this for register 0xFE (the pointer is 0xFE, not 0x55). It does not for the other four bytes. hekate has worked this way on every Switch, so the window is probably not real, but T1 cannot show it. | Wait one transfer time (about 250 microseconds at 100 kHz for address and one byte) with `now_us` before the first `STATUS` read, and treat a data byte equal to the pointer just written as suspect. No allowlist change. |
| 4 | **`CONFIG_LOAD` 0x25 is only partly documented.** Bit 0 (master load) is in Linux, mainline and the Switchroot kernel. Bit 2 (timeout load) is defined there and never used, so its effect is not documented. Bit 5 is defined in neither; it is hekate's value alone. | No harm known: hekate writes 0x25 on every transfer on this console. | Keep 0x25 for the first run, because normal mode with this value is the combination that works on this board, and list bit 5 as unsourced. The alternative, 0x01 as Linux uses with packet mode, is untested with normal mode here. Reviewer's choice. |
| 5 | **A transfer that times out leaves the controller mid-transaction.** There is no abort in the allowlist: no FIFO flush, no bus clear. | See below. | No change: adding an abort adds writes. The later stage can add it. |

## What a timeout leaves behind

After `BUSY` stays set for 100 ms the loader reads `STATUS` and `INT_STATUS` once, shows the stop, waits 15 s and calls PSCI
`SYSTEM_RESET`. During that time the master may hold the transfer, and a slave may hold SDA or SCL. The datasheet says the
TMP451 releases a stuck bus only with its SMBus timeout (register 0x22 bit 7), which is off by default, or after a general
call reset. The reset restarts the SoC and the I2C1 controller. It does not power the TMP451 off. hekate's next `i2c_init`
runs the bus clear (nine SCL pulses and a stop condition), which is the recovery for a held SDA. If it does not work, the
sensor would not answer hekate either, and Nyx's fan loop would read zeros.

## Timer

`io_now_us` is the ARM generic counter, a hardware counter that runs from the 19.2 MHz system counter. It is not a software
loop count and not the SoC's TMR block. The host test uses a fake clock, so the 100 ms bound is shown in the model and not in
silicon. In code: `isb; mrs x8, CNTPCT_EL0`, then `(x * 5) / 96`, which is ticks divided by 19.2.

## Transfer order audit

`TransferOrderIsHekatesFromTheFirstWriteToTheLastRead` compares the whole write log with the sequence hekate's driver produces
for ten transfers: write transaction `CMD_ADDR0` 0x98, `CMD_DATA1` pointer, `CNFG` 0x2800, `CONFIG_LOAD` 0x25, `CNFG`
0x2A00; read transaction `CMD_ADDR0` 0x99, `CNFG` 0x2840, `CONFIG_LOAD` 0x25, `CNFG` 0x2A40; for registers 0xFE, 0x09, 0x0A,
0x00, 0x01 in that order. It passes. The allowlist alone could not show this. What is still unaudited: the real silicon's
reaction to the order, and that a separate write and read (a stop between them) is what the TMP451 accepts. The datasheet
says reads use the last pointer written, and hekate does the same.

## T2: one-shot from the datasheet

- Command: "writing any value to the one-shot start register, pointer address 0Fh"; "the value of the data sent in the write
  command is irrelevant". So a write of two bytes, pointer 0x0F then one data byte. The datasheet does not name the SMBus
  type, so a pointer-only write is not assumed to start a conversion. Allowlist additions: `CMD_DATA1` 0x000F, `CNFG`
  size-2 forms (0x2802, 0x2A02). Register 0x0F cannot be read.
- Completion: the part returns to shutdown when the cycle ends. BUSY (status bit 7) means "ADC is converting". The datasheet
  does not describe BUSY in one-shot mode specifically. Design: wait 35 ms (typical 34), then poll BUSY clear with the
  100 ms bound, then read. Conversion time is 31 ms minimum, 34 ms typical, with no published maximum.
- Remote diode open: status bit 2 OPEN, set by a comparator when D+ is above V+ minus 0.3 V. The reading in that state is
  not specified, so OPEN decides.
- Status read side effect: flags clear on a read once their condition is gone.
- Extra signals at no cost: the limit registers power up at 0x55 (85 C) for local and remote, so status RHIGH (bit 4) and
  LHIGH (bit 6) are set above 85 C unless something changed the limits. hekate does not.
- Remote offset: registers 0x11 and 0x12 are written by hekate's `tmp451_init` (0 on the Erista). Whether Nyx ran it before
  our entry matters for the meaning of the reading, and T2 reads them.

## T3: two stop paths

Keep cooling where cooling is what is wanted, and cut power where the fault may be in the fan circuit itself.

| Path | Used for | Action |
| --- | --- | --- |
| Keep cooling | rotation proven earlier, then: sensor invalid or unanswered, temperature at 55 C or more, stage deadline, loader failure, any stop of the diagnostic | leave the rail and the duty as they are until the reset call; no off sequence |
| Cut | rotation not yet proven, or: readback of the rail GPIO or `PWM_CSR_1` differs from what was written, edges with the rail off, edges with duty 0, tachometer count falling to zero after rotation was proven (a stalled motor with power on) | duty 0, rail off, pin parked, as hekate's `fan_set_duty(0)` and `regulator_5v_disable` |

Limit of "keep cooling": the fan stays on only until the reset. The PWM, rail and GPIO registers are SoC state, so PSCI
`SYSTEM_RESET` returns them to reset values and the fan stops until hekate's Nyx loop starts it (`tmp451_init`, then
`fan_set_from_temp`, from the source). That gap is not measured. Cooling that survives the reset needs the PMIC or hekate,
and nothing T3 does provides it.

## T4: where it stands

- MAX77620 watchdog: registers and the 2, 16, 64 and 128 s periods are from Linux. Expiry with `WD_RST_WK` set is a restart.
  **Whether `WDTEN` stays set after that restart is still not known.** The Maxim/Analog datasheet and the TI-style
  pages did not load here, and no Linux or hekate code states it. Not implemented, as asked.
- Tegra TKE watchdog: the Erista DTB lists `watchdog@60005100` (`nvidia,tegra-wdt-t21x`, timer index 7, expiry count 4) as
  disabled. The Switchroot kernel's `tegra21x_wdt.c` did not load, and the TRM is not available here. hekate's only use
  (WDT4, FIQ, 5 s) has no reset bit. Which WDT resets the system from an A57 stays open.
- SOC_THERM: offsets and fields from Linux (`THERMCTL_THERMTRIP_CTL` +0x80; masks above). PMC thermal reset
  (`PMC_SENSOR_CTRL` 0x1B0, `PMC_SCRATCH54/55` 0x258, 0x25C) is described in the 2014 patch for Tegra114 and 124. The Erista
  DTB carries no PMC reset command.
- Needed to go on: the TRM or the MAX77620 datasheet pages for the reset-source columns of `CNFGGLBL2` and `ONOFFCNFG2`, and
  the TKE WDT section.

## D1

Hardware boot stays on hold until T3 and T4 are verified.
