# SD diagnostic (D1-pre)

Two hekate entries that print what U-Boot says about the SD card, then reset. They run before D1 so the card can be
compared with what the exporter requires. No exporter, no loader, no kernel window, no write to the card or to RAM
beyond U-Boot's own state.

| Entry | Runs |
| --- | --- |
| `iOS Diag SD` | `echo devnum`, `mmc list`, `part list mmc 1` |
| `iOS Diag SD Info` | `mmc dev 1`, `mmc info` |

## Build

```sh
python3 tools/package/diag_sd.py build \
  --frame boot/diag/sd/u-boot-script.txt \
  --page1 boot/diag/sd/page-1-state.txt --page2 boot/diag/sd/page-2-info.txt \
  --boards kits/SwitchBoard/SwitchBoard.h --bl31 bazel/pinned/firmware/bl31.bin \
  --bl33 bazel/pinned/firmware/bl33.bin --out "$PWD/build/diag-sd"
```

Copy `bootloader/ini/ios-diag-sd*.ini` and `switchroot/ios-diag-sd*/` to the card's FAT. Run page 1 first.

## Why two pages

`mmc dev 1` starts the card again, `mmc info` on its own does not. The two commands come from U-Boot's `cmd/mmc.c`; the
pinned binary is a fork whose source was not read, so this is the upstream behaviour (v2018.01 matches the strings in the
binary, v2024.01 differs), not a measurement:

| Command | Effect |
| --- | --- |
| `mmc list` | prints device names; starts nothing |
| `part list mmc 1` | reaches the card as `read mmc 1` does, a start only if U-Boot has not done it, then reads the partition table |
| `mmc dev 1` | starts mmc 1 again even if it is up (the same call the distro `mmc_boot` makes before every Switchroot boot) and sets the current device |
| `mmc info` | with no current device it selects device 0 first, which is not the SD card, and starts it |

So `mmc info` never runs before `mmc dev 1` (a test enforces the order, and keeps `mmc info` off page 1). Page 2's card
state is the one after a restart, the same restart a normal Switchroot boot performs; page 1 is closer to what D1's
`read mmc 1` meets. If the two pages disagree about the card, report both.

`mmc info` prints no bus mode, so single data rate is read as `Bus Width: 4-bit` without a `DDR` suffix; `Tran Speed`
is shown but not judged.

## Compare with the exporter

`tools/package/sd_requirements.py` takes the text of page 2 and marks each exporter condition `MET`, `NOT MET` or
`NOT SHOWN`. Five can be judged from `mmc info` (SD card, high capacity, 512 byte blocks, 4 bit single rate, at most
2^32 sectors). Five cannot (OCR ready with CCS, CSD version 2, RCA and CID nonzero, CSD capacity equal to the MMC and
block descriptor values, initialized flag with the controller base `0x700b0000` and block device 1 on the exact
drivers); the exporter checks them in D1 and they stay open until then.

## Public fixtures

`mask()` replaces the maker, OEM and name lines. Do not publish the photograph, and never publish a CID, serial number or
CSD from D1's record; keep the typed, masked text only.

## Run

Charged battery, console on a hard surface, attended, filmed, cool at the start, off between pages. No total limit; each
stage has a deadline from what it runs.

| Stage | Runs | Deadline |
| --- | --- | --- |
| Entry selected to the first U-Boot line | hekate, BL31, U-Boot | 20 s |
| `mmc list`, `echo` | prints names | 2 s |
| `part list mmc 1` (page 1) | card start if not up, one sector read | 15 s |
| `mmc dev 1` and `mmc info` (page 2) | card restart, a few commands | 15 s |
| Countdown to hekate's menu | 15 s count, reset | 25 s |

Past a deadline, or if the console is hot: hold the power button and report the last line.
