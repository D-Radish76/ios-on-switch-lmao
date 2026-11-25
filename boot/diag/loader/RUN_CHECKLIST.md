# D0 and D0-R run checklist

For `iOS Diag Loader` (D0, run once on the Erista and passed) and `iOS Diag Loader R` (D0-R, run once and passed). One run, attended. Do not start it before the review that this file
belongs to has said go.

## Before

- [ ] The card's files are the ones the Mac verified. Run `shasum -a 256` on the card's copies and compare with the
      values the Mac build printed (the table gives prefixes only; the full values come from that build, and `bl31.bin`
      from `bazel/pinned/SHA256SUMS`). Any mismatch: do not run.
- [ ] The commit built is the reviewed one (`git log -1` on the Mac).

| File on the card | SHA-256 |
| --- | --- |
| `switchroot/ios-diag-loader/bl31.bin` | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` |
| `switchroot/ios-diag-loader/bl33.bin` | starts `b63f49b5` |
| `switchroot/ios-diag-loader/script.txt` | starts `f762d2a3` |
| `bootloader/ini/ios-diag-loader.ini` | starts `dd102bc6` |

- [ ] The experiment card only (the 32 GB one), backed up. `iOS`, `iOS Diag`, `iOS Diag Memory` entries untouched.
- [ ] Battery 100 % and the charger out; the console on a hard surface, vents clear, room temperature, no case.
- [ ] hekate 6.5.5, Auto Boot off. Nothing in `bootloader/ini` boots on its own.
- [ ] Phone recording the screen from before `More configs` until the screen returns to hekate or you stop.
- [ ] You can reach the power button and you are staying with the console.

## During

Expected screen: U-Boot lines `BOARD OK`, `FRAMEBUFFER OK`, `DTB COPIED AND FRAMEBUFFER RESERVED`, `LOADER WRITTEN TO
0xA0000000`, `HANDOFF TO THE LOADER`; then the screen clears and the loader's report appears (board row, 25 MC rows, 6
GIC words, FDT walk); `RESET IN 15 S. PSCI SYSTEM_RESET FROM THE LOADER IS NOT VERIFIED`; hekate.

| What you see | Do |
| --- | --- |
| The report appears and the console returns to hekate after about 15 s | Wait for the menu. Done. |
| Screen ends at `HANDOFF TO THE LOADER` and never clears | The cause is not known from the screen: a failed check that parks at EL2, an exception, a hang before or inside the loader, or a loader that runs but cannot draw. Treat it as a hang. Hold the power button until it is off and report the last line. |
| `REFUSED: <status>` from U-Boot | RAM may already have been written; see the table below for what each status leaves behind. Wait for the reset to hekate (about 15 s) or hold the power button. Report the line. |
| `ALL RANGES FREE` is absent or `REFUSED` appears in the loader's report | Report it. The 15 s countdown still applies. |
| `PSCI SYSTEM_RESET RETURNED`, `NO RESET`, `NO EL CHANGE` or `EXIT CALLED OUTSIDE EL1` | Hold the power button until it is off. |
| A stage runs past its deadline (table below), or the screen goes black | Hold the power button until it is off. Do not retry on the spot. |
| The console feels hot, or the screen shows garbage | Hold the power button now. |

Do not press anything else during the wait. Nothing in D0 manages the fan or the temperature; the fan is off.

### What a `REFUSED` status leaves in RAM

The script sets U-Boot variables (never `saveenv`) and writes RAM at `0x8D000000` and then `0xA0000000`, besides
U-Boot's own working state (`bootm` bookkeeping, the variables). It does not write to the SD card or to flash. A reset or power-off discards all of it.

| Status | Written before it |
| --- | --- |
| `WRONG_BOARD`, `CONSOLE_FAILED`, `CONTROL_FDT_FAILED`, `FRAMEBUFFER_PROPERTY_FAILED`, `FRAMEBUFFER_MISMATCH` | Variables only. |
| `DTB_COPY_FAILED` | The copy to `0x8D000000` may be partial. |
| `DTB_HEADER_FAILED`, `DTB_PADDING_FAILED`, `FRAMEBUFFER_RESERVE_FAILED` | The DTB copy at `0x8D000000`, possibly with its reserve map half edited. |
| `BOOT_ENVIRONMENT_FAILED`, `BOOT_HEADER_FAILED` | The DTB copy; the loader's range is untouched. |
| `IMAGE_LOAD_FAILED` | The DTB copy, and part of the loader at `0xA0000000` may be written. |
| `BOOT_PREP_FAILED`, `UNEXPECTED_BOOTM_RETURN` | The DTB copy and the whole loader at `0xA0000000`. |

A status not in this table: assume everything above was written.

### Stage deadlines

There is no total time limit. Each stage has a deadline chosen from what it executes: past it, a stall is more likely than
slowness, and the run is cut. Times from the first D0 run's video (selection to first U-Boot line, line to line, handoff to
report) replace these once read; until then they are the working figures.

| Stage | What runs | Deadline |
| --- | --- | --- |
| Entry selected to the first U-Boot line | hekate loads the payload and BL31 starts U-Boot | 20 s |
| Between two U-Boot progress lines | RAM copies (the DTB, 88 KB of loader), `bootm start`, `bootm loados`, `bootm prep`; no card access | 10 s |
| `HANDOFF` line to the loader's first line | the EL2 checks, the EL change, one screen clear | 10 s |
| First loader line to `RESET IN 15 S` | register reads, the uncached FDT walk, drawing | 20 s (D0-R draws about 14 more lines) |
| Countdown to hekate's menu | 15 s count, PSCI reset, hekate start | 25 s |
| Any refusal path | the 15 s wait and `reset` | 25 s from the `REFUSED` line |

Past a deadline: hold the power button. Report the last line and how long it had been there.

## After

- [ ] Keep the video. Transcribe the 25 MC rows and 6 GIC words into a `LOG.txt` as typed, with the date and commit.
- [ ] Power the console off and let it cool before the next attempt, whatever happened.
- [ ] Report the commit, the four hashes as read from the card, what the screen showed and whether hekate returned.
- [ ] No second run, no D1, no D2 and no kernel until that report has been reviewed. The Erista row stays `boot 0`.

## D0-R

Same card rules and stage deadlines as D0, entry `iOS Diag Loader R`, files under `switchroot/ios-diag-loader-r/` and
`bootloader/ini/ios-diag-loader-r.ini`; hashes are read from the Mac build of `//boot:diag_loader_r`. D0's entry and files
stay on the card unchanged.

Expected, beyond D0's screen: the title `LOAD DIAG D0-R`; left column rows 16 to 29: `BANK 0 0080000000 007FD00000 00FFD00000`
and `BANK 1 0100000000 0080000000 0180000000`; the reserve map entries as `MAP 0..2` (the DTB's `0x80000000..0x80020000` first)
and the node range `NODE 0` (`0xB0000000..0xB0200000`); `EXPORTER 00A7000000 00A7010000 CLEAR? BANK 0` and
`SD REFERENCE 00A001F000 00A0020000 CLEAR? BANK 0` (the font has no comma, so `CLEAR, BANK 0` is drawn with `?`). Transcribe every line. Any `OVERLAPS` or `OUTSIDE THE BANKS` is red:
report it and do not go on to D1.
