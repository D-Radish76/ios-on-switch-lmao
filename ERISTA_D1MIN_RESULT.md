# D1-min result: first hardware run

Run 2026-11-26 on the Erista, hekate 6.5.5, from Nyx "More configs", entry `iOS Diag D1-min`. The user reported it at 22:05 JST, read from the
photograph, which is not stored. A report at 22:11 JST corrected the sector line: the screen shows it in full, as the script echoes it, and
screen and script do not differ. The record is `boot/diag/erista-d1min-run1.txt`. Verdict: PASS, by the criteria set in `ERISTA_D1MIN_REVIEW.md`.

| Criterion | Result |
| --- | --- |
| The CRC32 U-Boot printed equals the Mac's CRC32 of sector 0 | `e1d4e0ff` and `E1D4E0FF`: equal |
| `55 AA PRESENT` for this card | present |
| A clean return to hekate by the loader's reset, no `REFUSED` | PSCI `SYSTEM_RESET` returned to hekate, no POWER hold |

Start 34.5 C, end 38.6 C in Nyx, +4.1 C. Both are below the provisional 40 C candidate. The rise covers hekate, Nyx and the run together, and the
run's duration was not reported, so the run's own share is unknown.

## What it shows

- U-Boot's SD host driver read block 0 of `mmc 1` into DRAM at `0xA001F200` on this console, and the 512 bytes equal what the Mac reads from the card.
  `mmc dev 1` and `mmc read` ran here for the first time and returned.
- The sector ends in `55 AA`.
- D0-R's loader, handed off after the read, drew its whole report: `EL1 STATE PASS`, 25 MC rows equal to the OLED's table, 3 reservations, a clean FDT
  walk, the exporter range and the SD reference range `CLEAR`. The read at `0xA001F200` did not disturb the SD reference range check: that check
  looks at the memory map, not at the bytes.
- The loader size, 88848 bytes, equals D0-S v1's record: it is D0-R's loader.
- Five runs of this loader family have now ended in hekate by the loader's own PSCI `SYSTEM_RESET`: D0, D0-R, T1-T, D0-S v2, D1-min.

## What it does not show

- **XNU's SD driver is unverified.** This run exercised U-Boot's driver, at EL2 before the handoff, with U-Boot's clocks, pinmux and DMA setup. It says
  nothing about `NativeSwitchBlock.cpp`, the SDMMC1 setup XNU would do, or any access after the handoff.
- Anything about the card's later sectors, a write, or the eMMC.
- That U-Boot prints the CRC zero padded. The value has no leading zero, so the question stays open.
- That the run's own heating is small: only a Nyx start and end reading exist.
- CRC32 is a check for corruption, not tamper protection.

`boot` stays 0 and the feature mask stays 0. T2, T3, T4, the full D1 and every XNU run stay on HOLD.
