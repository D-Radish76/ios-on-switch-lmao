# D0-S v2 result: first hardware run

Run 2026-11-26 on the Erista, hekate 6.5.5, from Nyx "More configs", entry `iOS Diag State V2`. The user reported it at 21:30 JST and added the register values and framebuffer attributes at 21:40 JST, read from the photograph.
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

## Measured values

| Register | Address | Width | Value |
| --- | --- | --- | --- |
| CAR `RST_DEVICES_L` | `0x60006004` | 32 | `049102CA` |
| CAR `CLK_OUT_ENB_L` | `0x60006010` | 32 | `9802D131` |
| SDMMC1 Present State | `0x700B0024` | 32 | `01FB0000` |
| SDMMC1 Clock Control | `0x700B002C` | 16 | `0007` |

Bit 14 (SDMMC1) is clear in `RST_DEVICES_L` and set in `CLK_OUT_ENB_L`, which is the open gate that `READS 4` implies. In Present State
the SDHCI card inserted and card state stable bits (16, 17) are set, the card detect pin level (18) is clear, the write protect pin level
(19) is set, the DAT lines (23:20) are all high and the CMD line (24) is high; command and data inhibit (1:0) are clear. In Clock
Control the internal clock enable, internal clock stable and SD clock enable bits (0, 1, 2) are set and the divider is 0. These are
field decodes from the SDHCI layout, not a statement about what the card is doing.

Framebuffer, from the same screen: `0xF5A00000`, 720 x 1280, `a8b8g8r8`, rotation 3, reserved `0x400000`, loader 88400 bytes. The first
five equal D0-S v1's record. The loader size is the number the script printed. The Linux flat build here was last measured before the 16 bit change, so it is not compared.

## What it does not show

- That the values repeat. One run, one reading, transcribed by a person.
- Anything about D0-S (v1). Its hang is still not explained: the instruction that hung is not identified. v2 has no device load in
  U-Boot, so v2's pass says nothing about the U-Boot loads v1 made, and does not show that they were the cause.
- A temperature rise. One end reading, no start reading.

`boot` stays 0, the feature mask stays 0 and every other HOLD is unchanged.
