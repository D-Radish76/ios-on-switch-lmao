# T3: fan rotation step

Design of 2026-11-27, revised after the 20:14 JST review. `//boot:diag_t3` implements it offline. It is not approved to run, not on any card and not run. `boot` stays 0, the feature mask 0, T3 and T4 are not waived, the first XNU run is on HOLD, T3-R and T4-R are not run again.

Marks: **measured** (T3-R run of 2026-11-27, photograph), **source** (hekate v6.5.5 read through a summarising fetch, quotes not byte-checked), **inferred**, **unknown**.

## Safety judgement

The review said to stop and report if safety could not be judged. It was judged, relative to hekate's own behaviour and with the open points below kept open. This is not a safety guarantee.

| Question | Answer | Mark |
| --- | --- | --- |
| Is the state T3 creates one hekate creates itself? | Yes. Nyx calls `fan_set_from_temp` every 5 s: from 42 C it sets duty 51, and the fan path enables the 5 V rail first (A5 high, and on the Erista CC4 low). T3 reaches the same register state with the same writes | source |
| What does T3 leave alone? | CC4 and `USB_VBUS_EN0`, `SATA_LED_ACTIVE`, every PMC word, the CAR clock and reset words, `PWM_CSR_0` (backlight), the tach and rail pinmux words, every GPIO `CNF` and `OE` word | design, checked by the allowlist |
| What if the entry state differs from the measured one? | T3 reads 20 registers first and compares them with what hekate's enabled state needs. Any difference ends the run before the first write | code |
| CC4 | Its role is **unknown**. hekate labels the line "USB 5V VBUS EN" in the T210 branch and drives it low on enable, and does not say what low means. T3 requires it low and never writes it, so it is no different from hekate's own fan use | source |
| Other loads on the A5 rail | **unknown**. A Joy-Con on the rail or a USB device would see 5 V, as with hekate's fan. The run sheet requires both detached. Fan current is not measured | unknown |
| `PWR_DET_VAL` bit 21 | The photograph's digits are unreliable. T3 reads it itself and writes nothing if it is set | code |
| After `SYSTEM_RESET` | **Not claimed** to restore any register or pin. The off sequence is the only restore, and each of its writes is read back | code |
| A hang with the rail on | No timeout can leave a hung bus access. The rail limit is checked in the tach loop and between phases, which covers a slow run, not a hang. POWER hold is the only recovery and is unproven for that state | unknown |

Go: the writes are the ones hekate makes in normal operation, under checked preconditions, with an off sequence that always runs. What stays open is absolute: CC4 polarity, rail loads, fan current, behaviour of the rail pin across a reset. The owner decides whether that is enough for a run.

## Changes from the first design

| Review point | In the code |
| --- | --- |
| Duty 51 only | The duty 76 word is not in the allowlist or in the image, and a test checks that the gate refuses it |
| Continue off processing after a readback mismatch | Every off write that undoes a sent write runs, in the order PWM, rail, pad, whatever stopped the run; a mismatch sets the stop code and a mismatch bit and nothing else |
| Rail limit inside the tach loop | The loop asks `rail_expired` on every pass; it also runs between phases and before each write. 15 s from the rail write |
| No claim that `SYSTEM_RESET` restores registers | Dropped from the design and printed on the screen |
| Tach level not fixed to 0 | The state check does not look at the level. Each window records its first and last level and counts changes; the verdict compares counts between windows |
| Rail and CC4 re-checked | The section above |

## Where the state at our entry comes from

The owner starts every diagnostic from Nyx. The chain, from the sources:

| Step | What happens to the fan block | Mark |
| --- | --- | --- |
| Nyx `_launch_hos` calls `hw_deinit(false)`; Nyx defines `BDK_HW_EXTRA_DEINIT` | `fan_set_duty(0)` writes `PWM_CSR_1` = `0x81000000`, then `regulator_5v_disable(ALL)` drives A5 and CC4 low; `LCD_GPIO2` goes to `0x74` | source |
| Nyx jumps to hekate's `ipl_main` | `display_backlight_pwm_init()` runs after `gfx_con_init()`: it calls `clock_enable_pwm()` and writes `PWM_CSR_0` | source |
| `clock_enable` (shared helper) | puts the module into reset first, enables the clock, waits 2 us, releases reset | source |
| `launch_l4t` calls `hw_deinit(sld)`; the hekate build does not define `BDK_HW_EXTRA_DEINIT` | no fan, 5 V, PWM or pinmux call | source |
| BL31 and the pinned U-Boot (`bl33.bin`) | a scan of the pinned `bl33.bin` found no `0x7000A0xx` literal, no `movz`/`movk` pair for it and no CAR literal; its DTB lists `pwm@7000a000`. A literal and pair scan is not exhaustive | inferred |
| Our loader | **measured**: PWM clock on and reset released (the PWM word was read), `PWM_CSR_1` `0x00000000`, `LCD_GPIO2` `0x74`, `CAM1_PWDN` `0x59`, A5 and CC4 `OUT` 0 | measured |

The reset in the third row is the likely reason `PWM_CSR_1` is zero: the whole PWM block, both channels, goes through reset once per hekate start, after Nyx wrote its off word. That is **inferred**: it fits the source and the one reading, nothing in this chain was observed separately. The first T3-R design predicted `0x81000000` because it did not follow `ipl_main` this far. Mainline-style PWM documentation and the reset value of the register are not available here (the TRM is not in the repository), so "zero is the reset value" is a fit, not a fact.

## Initial state T3 would assume

Each line must be read again by T3 itself before any write. A difference stops the step without a write.

| Register | Address | Value seen | Used for |
| --- | --- | --- | --- |
| `PWM_CSR_1` | `0x7000A010` | `0x00000000` | channel 1 starts disabled; T3 writes the whole word |
| `LCD_GPIO2` pinmux | `0x7000320C` | `0x00000074` | fan PWM pad parked, tristate, pull down |
| `CAM1_PWDN` pinmux | `0x700031EC` | `0x00000059` | tach pad already input with pull up, so no write |
| `SATA_LED_ACTIVE` pinmux | `0x7000304C` | `0x00000001` | rail enable pin function, already set, so no write |
| `USB_VBUS_EN0` pinmux | `0x700031A8` | `0x00000101` | already the value hekate writes, so no write |
| GPIO A | `0x6000D000`, `D010`, `D020` | `0x20`, `0x20`, `0x00` | A5 is a GPIO output driven low (rail off) |
| GPIO CC | `0x6000D700`, `D710`, `D720` | `0x18`, `0x18`, bit 4 `0` | CC4 is a GPIO output driven low |
| GPIO S | `0x6000D408`, `D418`, `D438` | S7 `CNF` 1, `OE` 0, `IN` 0 | tach pin is an input; its level is read and not required to be 0 |
| GPIO V | `0x6000D504`, `D514` | `0x06`, `0x07`; V4 bits 0 | V4 already special function |
| PMC `NO_IOPOWER` | `0x7000E444` | `0x00000000` | the GPIO bit is already clear, so no write |
| PMC `PWR_DET_VAL` | `0x7000E4E4` | bit 21 not confirmed | hekate clears this bit; the photograph's digits are unreliable, so it is re-read before it is trusted |
| CAR `RST_DEVICES_L`, `CLK_OUT_ENB_L` | `0x60006004`, `0x60006010` | gate open, raw words not seen | the fixed T3-R image shows them; T3 reads them and stops if the gate is not open |

Consequence: if all of this holds at run time, hekate's whole `regulator_5v_enable` and fan init reduce to three writes. Nothing else in hekate's fan code has to be repeated, because the handoff already holds its result.

## Write list

Every write is 32 bit and is followed at once by a 32 bit read of the same register. GPIO A is written as a whole word: the state check requires `GPIO_A_OUT` to be zero first, so `0x20` sets A5 and `0x00` returns the register to what it was. No read-modify-write is needed and none is made.

| # | Phase | Register | Write | Basis |
| --- | --- | --- | --- | --- |
| W1 | rail on | `0x6000D020` | `0x00000020` | `regulator_5v_enable`: A5 high |
| W2 | duty 51 | `0x7000A010` | `0x80B90000` (`EN`, `236 - 51 = 185 = 0xB9`) | `fan_set_duty(51)` |
| W3 | duty 51 | `0x7000320C` | `0x00000001` | route PWM1 to the pad |
| O1 | off | `0x7000A010` | `0x81000000` | hekate's off word |
| O2 | off | `0x6000D020` | `0x00000000` | rail off |
| O3 | off | `0x7000320C` | `0x00000074` | pad parked as found |

O1 runs only if W2 was sent, O2 only if W1 was sent, O3 only if W3 was sent. After the off sequence `PWM_CSR_1` holds `0x81000000`, not the `0` found at entry; that is hekate's own off state.

## Phases, windows and verdict

1. State: 20 reads, then the check. The first difference stops the run with no write.
2. Idle window, 2 s, rail off.
3. Rail on (W1), read back, window 2 s.
4. Duty 51 (W2, W3), read back, spin-up window 1 s, spin window 2 s.
5. Off (O1, O2, O3), each read back.
6. Stopped window, 2 s, rail off.

A line is drawn and settled before each phase starts and again when it ends. A line that does not draw stops the run before the next write; with the rail on, the off sequence follows.

A window polls the tach pad (`GPIO_S_IN` bit 7) as fast as the loop goes and counts level changes and samples. Polling breaks the repository's rule to prefer interrupts. It is bounded to short windows with no kernel running, and an interrupt path needs GIC and GPIO interrupt writes that no run has tried. The review decides.

Pass: the spin window has at least 4 changes, more than the idle window and the rail window, and each of the three has at least 1000 samples. No absolute rpm is claimed. A count with too few samples is no verdict. No pass runs the off sequence and reports; there is no retry and no higher duty.

## Time limits

All on `CNTPCT` at 19.2 MHz, provisional and not measured on the console: state reads 20 s, windows 2 s and 1 s, rail on 15 s at most, a sample limit per window. A counter at another frequency makes every wait and limit count as spent. A person holds POWER if no new line appears for 30 s. These are not safety guarantees.

## Unknown, and what would settle it

| Item | Settled by |
| --- | --- |
| Reset value of `PWM_CSR_1`, bit meaning of `0x100 << 16`, edge rate per revolution | the TRM, or the fan's own count |
| CC4 polarity and what hangs on the rail | the schematic, or a bench measurement by the owner |
| Whether `PWR_DET_VAL` bit 21 is clear at entry | T3's own state read |
| Whether `PWM_CSR_1` and the pad word read back exactly as written | a run, after approval; if not, T3 stops and runs the off sequence |
| U-Boot's and BL31's PWM handling | source of the pinned `bl33.bin`, not available; the offline scan is partial |
| Behaviour of the rail pin across `SYSTEM_RESET` | a run, after approval |

## Order of work

1. The owner reviews this design, the write list and the safety judgement.
2. A Mac check of the T3 commit: build, host tests, audit, `diag_images`.
3. Run sheet, card procedure and approval in `ERISTA_T3_RUN.md`, naming what is plugged into the rail.
