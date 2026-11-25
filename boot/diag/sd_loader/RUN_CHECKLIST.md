# D1 run checklist

For `iOS Diag Loader SD`. One run, attended, on the 32 GB experiment card. Do not start it before the review this file
belongs to has said go. D1 hardware boot is not approved yet.

## Before

- [ ] The Mac's `shasum -a 256` of `bazel-bin/boot/diag_loader_sd/package/**` matches the card's copies, and
      `bl33.bin` matches the value the reviewed build printed. Any mismatch: do not run.
- [ ] `//boot:diag_loader` and `//boot:diag_loader_r` hashes are the ones from the D0 and D0-R runs.
- [ ] The commit built is the reviewed one (`git log -1`).

These do not depend on the compiler and can be checked from the repository (`tools/package/diag_loader.py build` output):

| File on the card | SHA-256 |
| --- | --- |
| `switchroot/ios-diag-loader-sd/bl31.bin` | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` |
| `switchroot/ios-diag-loader-sd/script.txt` | `23ba8ef408819e698eb6d7d616d3dff40b927ed37ae812933c868aeba8eeae0c` |
| `bootloader/ini/ios-diag-loader-sd.ini` | `87a7e040bbcfe5fa337a4fa97b9f3bfcee39f940e151cf8b58760a1f967622f4` |
| `switchroot/ios-diag-loader-sd/bl33.bin` | `aedd7b951a3037dd2c27d70cb3326e7284419c4b8c3a9d801944aac8c772b4a0` (Mac build, 2026-11-25; it holds the Xcode-built loader) |

- [ ] Experiment card only, backed up. The `iOS`, `iOS Diag`, `iOS Diag Memory`, `iOS Diag SD`, `iOS Diag Loader` and
      `iOS Diag Loader R` entries are untouched.
- [ ] Thermal: re-read `ERISTA_D1_DESIGN.md` "Temperature". The fan is off, no sensor is read and the hardware trip is not
      known to be armed. Battery 100 %, charger out, hard surface, vents clear, cool console, no case. If the run
      stops abnormally (no reset to hekate), hold the power button at once; do not wait.
- [ ] hekate 6.5.5, Auto Boot off. Phone recording from before `More configs` until hekate is back.
- [ ] You can reach the power button and you stay with the console.

## During

Expected U-Boot lines: `BOARD OK`, `FRAMEBUFFER OK`, `DTB COPIED AND FRAMEBUFFER RESERVED`, `SD EXPORTER LOADED AND
CHECKED`, then `## Starting application at 0xAA001920 ...` and `## Application terminated, rc = 0x1`, the same pair for
`0xAA001A64`, `LOADER WRITTEN TO 0xA0000000`, `READ SD SECTOR 0`, `## Starting application at 0xA7000000 ...` and
`## Application terminated, rc = 0x0`, `HANDOFF TO THE LOADER`. The two `rc = 0x1` are the helpers returning 1, as read from the
binary; a helper line with `rc = 0x0` means other code ran: hold the power button. Then the loader's report: D0-R's rows,
the FDT reservation display, and the D1 lines:

| Line | Good |
| --- | --- |
| `SD STEPS n OF 10. EXPORTER RETURNED 0 PER SCRIPT` | 10 OF 10 |
| `CRC32 WANT .. BYTES 5E50` | `630D1CB8` |
| `CRC32 SLOT .. RUN .. GO ..` | all `630D1CB8` |
| `SD RECORD AND REFERENCE: VALID` | `FAILED CODE n NAME` otherwise (2 header, 3 padding, 4 crc, 5 changed, 6 card) |
| `CARD SECTORS .. OCR .. RCA ..`, `CSD STRUCT .. FLAGS .. SIG 55AA. ID NOT SHOWN` | capacity 29.8 GiB, OCR `C0..`, CSD structure 1 |

Nothing prints the CID, the CSD words or a U-Boot address. Do not photograph a screen that does.

| What you see | Do |
| --- | --- |
| Report appears, hekate returns after about 15 s | Done. |
| `REFUSED: <status>` | RAM may be written (table below). Wait for the reset (about 15 s) or hold the power button. Report the line. |
| Screen stops at `## Starting application at 0xA7000000` and stays for 10 s | The exporter's first `go` may hang (a loaded payload run for the first time; a stale I-cache line is the case in `ERISTA_CACHE.md`). A hang does not reset by itself. Hold the power button at once. Report the last line. |
| Screen stops after `READ SD SECTOR 0` for 15 s | The card read did not finish. Hold the power button at once. |
| Screen stops at `HANDOFF TO THE LOADER` and never clears | As in D0: treat as a hang. Hold the power button. |
| `SD STATUS BLOCK MISSING` or `SD STEPS` under 10 | Report it. The 15 s countdown still applies. |
| Any stage past its deadline, black screen, hot console, garbage | Hold the power button now, for at least 12 s (Nintendo's force-off time; not tried against this firmware). Do not retry on the spot. |

### Stage deadlines

| Stage | Deadline |
| --- | --- |
| Entry selected to the first U-Boot line | 20 s |
| Between two U-Boot progress lines (RAM copies, `bootm`, CRC32, no card) | 10 s |
| `READ SD SECTOR 0` line to the next line (one block read, card initialization) | 15 s |
| `go 0xA7000000` to the next line | 10 s |
| `HANDOFF` line to the loader's first line | 10 s |
| First loader line to `RESET IN 15 S` | 20 s |
| Countdown to hekate's menu, or any refusal path to hekate | 25 s |

### What a `REFUSED` status leaves behind

Nothing writes the card's data or flash. U-Boot variables are set, never saved. A reset or power-off discards RAM.

| Status | Written before it |
| --- | --- |
| `WRONG_BOARD`, `CONSOLE_FAILED`, `CONTROL_FDT_FAILED`, `FRAMEBUFFER_PROPERTY_FAILED`, `FRAMEBUFFER_MISMATCH` | Variables only. |
| `DTB_COPY_FAILED`, `DTB_HEADER_FAILED`, `DTB_PADDING_FAILED`, `FRAMEBUFFER_RESERVE_FAILED` | The DTB copy at `0x8D000000`, possibly partial. |
| `STATUS_BLOCK_FAILED`, `SD_REFERENCE_WRONG_DEVICE` | The above; the status block at `0xA700F000` may be partly cleared. |
| `EXPORTER_SLOT_HEADER_MISMATCH`, `EXPORTER_SLOT_CRC_MISMATCH`, `CACHE_HELPER_MISMATCH` | The above plus status words. Nothing loaded. |
| `EXPORTER_HEADER_FAILED`, `EXPORTER_LOAD_FAILED` | The above; the exporter may be partly at `0xA7000000`. |
| `EXPORTER_IMAGE_CORRUPT`, `CACHE_HELPER_RETURN_UNEXPECTED` | The exporter at `0xA7000000`. The helpers touch only the I-cache. |
| `BOOT_ENVIRONMENT_FAILED`, `BOOT_HEADER_FAILED`, `IMAGE_LOAD_FAILED`, `BOOT_PREP_FAILED` | The above, and the loader (whole or partial) at `0xA0000000`. |
| `SD_REFERENCE_CLEAR_FAILED` | The above; the reference page at `0xA001F000` may be partly cleared. |
| `SD_REFERENCE_READ_FAILED` | The above. U-Boot's driver has initialized the card; up to 512 bytes at `0xA001F200`. No `go`. |
| `EXPORTER_IMAGE_CHANGED` | The above, sector 0 at `0xA001F200`. No `go`. |
| `SD_CARD_EXPORT_FAILED`, `SD_CARD_RECORD_FAILED` | The exporter ran; its record at `0xA001F400..0xA001F500` may be written. |
| `SD_REFERENCE_METADATA_FAILED`, `UNEXPECTED_BOOTM_RETURN` | All of the above and the reference header. |

A status not here: assume all of it was written.

### SD access, as recorded

- Explicit data read: sector 0 of `mmc 1`, 512 bytes, once.
- U-Boot's driver issues its own card-initialization commands and changes controller state during that read. They are not
  separately observable from the script and are not counted as data reads.
- The exporter issues no card command and no MMIO; it reads U-Boot's cached objects.
- No retry, no `mmc0`, no `mmc dev`, no data write to the card.

## After

- [ ] Keep the video. Transcribe the D1 lines as typed into `LOG.txt` with the date and commit. No identifying values.
- [ ] Power off and let the console cool before any next attempt, whatever happened.
