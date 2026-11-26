# D0-S v2 result: first hardware run

Run 2026-11-26 on the Erista, hekate 6.5.5, from Nyx "More configs", entry `iOS Diag State V2`. The user reported it at 21:30 JST.
The record is `boot/diag/erista-d0sv2-run1.txt`. The photograph is not stored in the repository.

The screen ended with `S2 COMPLETE. READS 4. STOP 0000`. The loader's PSCI `SYSTEM_RESET` returned the console to hekate by itself;
no POWER hold was needed. Nyx showed 35 C afterwards. The start temperature was not recorded. Verdict: PASS.

## What the run shows

| Fact | From |
| --- | --- |
| The EL1 loader reached S2 and ran its steps to the complete line | the complete line |
| Four device reads were made and none was refused or left unshown (stop `0000`) | `READS 4`, `STOP 0000` |
| SDMMC1's clock was on and its reset was off when CAR was read | four reads happen only behind that gate (`s2.c`); a closed gate gives `READS 2` |
| The 32 bit reads of `0x60006004`, `0x60006010` and `0x700B0024` and the 16 bit read of `0x700B002C` completed at EL1 | the same |
| PSCI `SYSTEM_RESET` from the loader returned to hekate | the report, as in the D0, D0-R and T1-T runs |

## What it does not show

- The values. The CAR words, Present State and Clock Control were not reported, so nothing is recorded for them.
- That the 16 bit read returns what the controller holds. It returned and did not hang.
- Anything about D0-S (v1). Its hang is still not explained: the instruction that hung is not identified. v2 has no device load in
  U-Boot, so v2's pass says nothing about the U-Boot loads v1 made, and does not show that they were the cause.
- A temperature rise. One end reading, no start reading.

`boot` stays 0, the feature mask stays 0 and every other HOLD is unchanged.
