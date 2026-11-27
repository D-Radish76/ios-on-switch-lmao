# T3: fan rotation step, design after the first T3-R run

Design of 2026-11-27, updating the T3 section of `ERISTA_THERMAL_DESIGN.md` with what `ERISTA_T3R_RESULT.md` measured. Nothing here is implemented, approved or run. No fan, PWM, GPIO, pinmux or PMC write code exists, and none is written by this round. `boot` stays 0, the feature mask 0, T3 and T4 are not waived, the first XNU run is on HOLD, T3-R is not run again.

Marks: **measured** (T3-R run of 2026-11-27, photograph), **source** (hekate v6.5.5 read through a summarising fetch, quotes not byte-checked), **inferred**, **unknown**.

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
| GPIO S | `0x6000D408`, `D418`, `D438` | S7 `CNF` 1, `OE` 0, `IN` 0 | tach pin is an input, one sample low |
| GPIO V | `0x6000D504`, `D514` | `0x06`, `0x07`; V4 bits 0 | V4 already special function |
| PMC `NO_IOPOWER` | `0x7000E444` | `0x00000000` | the GPIO bit is already clear, so no write |
| PMC `PWR_DET_VAL` | `0x7000E4E4` | bit 21 not confirmed | hekate clears this bit; the photograph's digits are unreliable, so it is re-read before it is trusted |
| CAR `RST_DEVICES_L`, `CLK_OUT_ENB_L` | `0x60006004`, `0x60006010` | gate open, raw words not seen | the fixed T3-R image shows them; T3 reads them and stops if the gate is not open |

Consequence: if all of this holds at run time, hekate's whole `regulator_5v_enable` and fan init reduce to three writes. Nothing else in hekate's fan code has to be repeated, because the handoff already holds its result.

## Write list (design, not code)

Every write is 32 bit, followed at once by a 32 bit read of the same register compared with what was written; a mismatch runs the off sequence. Writes to GPIO bits are read-modify-write with the read-back after, the way hekate's `gpio_write` does it (the masked registers at `+0x80`, `+0x90`, `+0xA0` are not used by hekate and are not proposed).

| # | Phase | Register | Write | Basis |
| --- | --- | --- | --- | --- |
| W1 | rail on | `0x6000D020` | set bit 5 | `regulator_5v_enable`: A5 high |
| W2 | duty 51 | `0x7000A010` | `0x80B90000` (`EN`, `236 - 51 = 185 = 0xB9`) | `fan_set_duty(51)`, lowest step of Nyx's curve |
| W3 | duty 51 | `0x7000320C` | `0x00000001` | route PWM1 to the pad |
| W4 | optional step | `0x7000A010` | `0x80A00000` (duty 76) | next step of the curve, only after W2 and W3 showed edges |
| O1 | off | `0x7000A010` | `0x81000000` | hekate's off word |
| O2 | off | `0x6000D020` | clear bit 5 | rail off |
| O3 | off | `0x7000320C` | `0x00000074` | pad parked, tristate, pull down |

W3 follows W2 because hekate writes the word first; both happen with the rail already on. The off sequence runs in the order O1, O2, O3 and runs in every ending: a pass, a failed readback, no edges, a spent budget, a failed screen settle. After O3 the loader goes to the shared exit, whose `SYSTEM_RESET` returns every register to its reset state; that is the only restoring step beyond the off sequence, and no run has returned to hekate after a rail was on.

Not written, by design: the CAR reset and clock bits (a reset pulse such as `clock_enable` would zero `PWM_CSR_1` and also `PWM_CSR_0`, which holds the backlight), `PWM_CSR_0`, any PMC register, the pinmux words of the rail and the tach, GPIO CNF and OE. If any of those did not match, the state is not the one this design covers and T3 stops before W1.

## Tachometer, readback and time

Order of a run, after the same screen conventions as T3-R (each line shown and settled before the step it announces):

1. Read the table above. Stop with no write on any difference or a closed PWM gate.
2. Count S7 edges for 2 s with the rail off and PWM off: expected none. Edges here mean the pin picks up noise or another source, and the counts below would not be comparable.
3. W1, settle, count 2 s: rail on, fan not driven.
4. W2, W3, 1 s spin-up, count 2 s.
5. Optionally W4, 1 s, count 2 s.
6. O1, O2, O3, read back each, one more 2 s count after a short settle to show the fan stopped.
7. Shared exit.

Counting follows hekate's `fan_get_speed`: poll S7 (`0x6000D438` bit 7) and count a low to high transition each time, two edges per revolution (rpm is not claimed). The loop also counts its own iterations, so the screen shows how many samples a window got; a count taken with too few samples says nothing. Polling breaks the repository rule to prefer interrupts. For this diagnostic it is bounded to 2 s windows with no kernel running; an interrupt path would need GIC and GPIO interrupt writes that no run has tried, which adds risk for a one-time measurement. The review decides.

Time limits (provisional, none measured): each window is 2 s of `CNTPCT` at 19.2 MHz; the rail may stay on at most 15 s from W1, after which the off sequence runs whatever else is pending; the whole step is capped like T3-R at 20 s for its reads plus the 15 s dwell. A person holds POWER if no new line appears for 30 s. These are not safety guarantees.

Pass means: the edge count at duty 51 is above zero while the counts in steps 2 and 3 were zero. A higher count at duty 76 is reported, not required (the polling can lose edges, and the fan's range is unknown). No edge at duty 51: off sequence and report, no retry, no higher duty. No absolute rpm is claimed.

## The shared 5 V rail

Source: `regulator_5v.c` has a comment that A5 is "Fan and Rail power from battery 5V regulator EN". Neither the rail connector's consumers nor the fan's current are known here. What follows is open:

- **Other loads on the same enable.** A5 high powers more than the fan; a Joy-Con on the rail or a USB device would see 5 V. Whether the owner's setup has either attached is unknown. The run sheet must state what is attached, ideally nothing.
- **USB VBUS (CC4, `USB_VBUS_EN0`).** On the Erista hekate also switches this line when it enables the rail. At our entry it is already low with the pinmux word hekate writes; T3 leaves it alone. Whether low means VBUS off or a source select is not settled by this round.
- **Order of the enable.** hekate's first enable also sets the source pins and PMC bits; those match at entry, so T3 does not repeat them. If a run shows otherwise, W1 is not safe to assume.
- **After a reset with the rail left on.** A SoC reset puts A5 back to its reset state; what the pad does then is unknown. That is why the off sequence comes first.
- **A hang with the rail on.** Not covered by any timeout; POWER hold is the only recovery and is unproven for that state.

## Unknown, and what would settle it

| Item | Settled by |
| --- | --- |
| Reset value of `PWM_CSR_1`, bit meaning of `0x100 << 16`, edge rate per revolution | the TRM, or the fan's own count |
| Whether `PWR_DET_VAL` bit 21 is clear at entry | a reading by T3, or the fixed T3-R image |
| The raw CAR words | the fixed T3-R image, or T3's own read |
| Fan current, rail loads, spin-up time | not measurable by the loader; a bench measurement by the owner would |
| U-Boot's and BL31's PWM handling | source of the pinned `bl33.bin`, not available; the offline scan is partial |
| Behaviour of the rail pin across `SYSTEM_RESET` | a run, after approval |

## Order of work

1. The owner reviews this design and the write list.
2. A Mac check of the fixed T3-R image. A T3-R re-run is forbidden now; T3 reads every register it needs itself, so a re-run is optional and is the owner's decision.
3. Implementation as a new image with its own allowlist (21 reads, 7 writes at most), audit, host tests and hash record, ten earlier hashes unchanged. Not begun.
4. Run sheet, card procedure and approval as for T3-R, with what is plugged into the rail named.
