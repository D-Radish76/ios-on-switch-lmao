# D1-min: review before a run

This is the offline review of 2026-11-26. D1-min then ran once and passed (`ERISTA_D1MIN_RESULT.md`). `mmc read` has now run on this console. `boot` stays 0, the feature mask stays 0, every HOLD stays.
`diag_d1min_audit_test` pins what this page says about the script and the target.

D1-min is D0-R's loader, unchanged, behind a U-Boot script that adds one block between `bootm loados` and `bootm prep`: `mmc dev 1`,
one read of sector 0 into RAM, the CRC32 of those 512 bytes and a test of the last two bytes. The loader draws D0-R's report as it
does in D0-R.

## What was asked, and what the script does

| Question | Answer | Where |
| --- | --- | --- |
| `mmc dev 1` is the only device selected | yes. It is `sdhci@700b0000`, SDMMC1, the SD card. The eMMC is mmc 0 (`sdhci@700b0600`) and no command selects it | `u-boot-script-d1min.txt`; `erista-sd-pages.log` lines 6 to 8 |
| Only sector 0 is read | `mmc read 0xA001F200 0 1`: block 0, count 1, 512 bytes, into RAM | the script |
| CRC32 | `crc32 0xA001F200 0x200` prints the CRC32 of those bytes. It is recorded, not compared: no expected value exists, because the card's contents are not known to the script | the script |
| The `55 AA` signature | `itest.w *0xA001F3FE == 0xAA55`: the halfword at offset 510, little endian, is the bytes `55 AA` | the script |
| No write to the card or the eMMC | the only `mmc` commands are `mmc dev 1` and `mmc read`; there is no `write`, `erase`, `part`, `rescan`, `info` or other `mmc` verb, no `mmc dev 0`, no `saveenv`, `fatwrite`, `ext4write`, `sf` or `nand`. `mmc dev 1` restarts the card with commands but writes no data block | audit test |
| The sector is never shown | no `md`, no `echo` of the bytes. Only the CRC and the signature result are printed | audit test |
| The existing D0-R loader | `diag_d1min` has D0-R's `srcs`, `hdrs`, `defines`, linker script and board; only its entry (`ios-diag-d1min`), title and script differ | audit test; `ERISTA_FIRST_XNU_BOOT.md` |
| Existing hashes | the Mac gave `cdc52c65...` for D0-S, D1-min and D0-R: one `loader.uboot.bin`. Linux flat hash `c101d9c3...` for D0-R is unchanged in `erista-diag-image-hashes.txt` | the Mac report; the hash file |
| RAM written | the D0 writes (DTB copy at `0x8D000000`, one reserve-map entry, the loader at `0xA0000000`) and the 512 bytes at `0xA001F200` | audit test |

`0xA001F200` is the same address the normal loader script reads sector 0 to (`boot/u-boot-script.txt:166`) and lies at or above the
loader's ceiling `0xA001EF00` (`link.ld`), so the read cannot land on the loader. The loader's `SD REFERENCE` line checks that
`0xA001F000` to `0xA0020000` is free of the memory it maps and reserves; it does not read the bytes there, so the sector does not change
that line.

`CRC32 verification` here means a value the person can check, not a pass test. A host-side check of the same card, read only, is the
CRC32 of the card's first 512 bytes (`dd if=/dev/rdiskN bs=512 count=1 | python3 -c 'import sys,zlib;print("%08x"%zlib.crc32(sys.stdin.buffer.read()))'`,
with N the card's disk number and the card unmounted). U-Boot's `crc32` is the IEEE CRC32, which `zlib.crc32` computes. Whether the
digits U-Boot prints are zero padded is not known for this build. U-Boot prints the CRC, before the screen clears; the loader does not.

## Findings

1. **`mmc read` had not run on this hardware before the run (it has now, and returned).** `mmc dev 1` has (`erista-sd-pages.log`: the card initialized, `mmc1 is current device`, hekate
   came back by itself). The normal script reads with `read mmc 1 ...`, a different command, and that has not run on an Erista either.
   The read moves data through U-Boot's SD host driver into DRAM by DMA, which is new on this console.
2. **D0-S v1's hang is unexplained.** It happened after `bootm prep`, on U-Boot loads of device words. D1-min's U-Boot access to SDMMC1 is
   U-Boot's own driver and comes before `bootm prep`, as D1-pre's `mmc dev 1` page did; that differs from D0-S v1 but does not show
   the cause is absent. D0-S v2's pass says nothing about U-Boot's accesses, since v2 makes none.
3. **The card is restarted.** `mmc dev 1` re-initializes the card even though hekate left it initialized (`boot/diag/sd/README.md`). D1-pre
   did the same on this card and nothing changed on it.
4. **No stop other than the person.** A hang in `mmc dev` or `mmc read` has no timeout in the script (U-Boot's driver has its own) and
   no hardware reset (`ERISTA_STOP_PATHS.md`).
5. **Doc inaccuracy, corrected.** `ERISTA_NEXT_DIAGNOSTICS.md` said the loader prints the CRC32. U-Boot's script prints it, and that file now says so.

## Expected time, stop conditions, recovery

About 40 s from the first U-Boot line to hekate (an estimate from D0's own run; no D1-min run exists): U-Boot's steps, `mmc dev 1` and the
read, `sleep 5`, the loader's report, then 15 s and PSCI `SYSTEM_RESET`, which returned to hekate in the D0, D0-R, T1-T and D0-S v2 runs.

| What you see | Do |
| --- | --- |
| `D1-MIN SECTOR 0 READ TO 0xA001F200`, a CRC32 line, `SIGNATURE 55 AA PRESENT` or `ABSENT`, D0-R's report, hekate after about 15 s | Done. |
| The screen stops at `LOADER WRITTEN TO 0xA0000000` for more than 20 s | a hang in `mmc dev 1` or `mmc read`. Photograph, hold POWER until off, report the last line |
| `REFUSED: SD_DEVICE_SELECT_FAILED` or `REFUSED: SD_SECTOR_READ_FAILED` | the script waits its dwell and resets. Nothing was written. Report; do not retry on the spot |
| The screen stops at `HANDOFF` or never clears | a hang. Hold POWER. Report the last line |
| `PSCI SYSTEM_RESET RETURNED`, `NO RESET`, `EXIT CALLED OUTSIDE EL1` | hold POWER |
| The console feels hot or the screen shows garbage | hold POWER at once |

The other `REFUSED` statuses are D0-S's (`ERISTA_D0S_RUN.md` section 5): `BOOT_ENVIRONMENT_FAILED`, `BOOT_HEADER_FAILED`, `BOOT_PREP_FAILED`,
`CONSOLE_FAILED`, `CONTROL_FDT_FAILED`, `DTB_COPY_FAILED`, `DTB_HEADER_FAILED`, `DTB_PADDING_FAILED`, `FRAMEBUFFER_MISMATCH`,
`FRAMEBUFFER_PROPERTY_FAILED`, `FRAMEBUFFER_RESERVE_FAILED`, `IMAGE_LOAD_FAILED`, `UNEXPECTED_BOOTM_RETURN`, `WRONG_BOARD`. Each stops before the
handoff and writes nothing. Recovery is the held power button, then hekate. Temperature: start below
40 C, note Nyx's reading before and after (the 40, 50 and 55 C figures are provisional). One run, no retry.

## Pass criteria for the first run (set 2026-11-26)

The run passes only if all of these hold:

1. The CRC32 U-Boot prints equals the CRC32 of the card's sector 0 taken on the Mac beforehand. Compare the numbers, not the strings: whether U-Boot pads the digits is not known.
2. The signature line says `55 AA PRESENT` for this card.
3. The run ends in hekate by the loader's own reset, with D0-R's report on screen and no `REFUSED` status.

A CRC mismatch, `ABSENT`, or a hang is a FAIL and is reported as it is. A match shows the read returned the card's bytes. CRC32 is not protection against deliberate change.

## Before a run, if approved

All of `ERISTA_FIRST_XNU_BOOT.md` "Before D0-S and D1-min", with these additions:

- The three loader hashes equal on the Mac.
- The card's CRC32 of sector 0 computed on the Mac first, and written down before the run.
- Nyx's start temperature read and written down before the run (start only below the provisional 40 C candidate). D0-S v2's start was not recorded, so its rise is unknown.
- The photograph from the first U-Boot line.

D1-min is a read-only diagnostic and does not approve D1-b, D1-c, D1, T2 or any XNU boot.
