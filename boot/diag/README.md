# Read only diagnostic

A second hekate entry, `iOS Diag`, that boots a U-Boot with one script and nothing else. The script reads the two
identity registers and the 25 memory controller registers `native_gate` (`boot/loader/loader.c`) compares, prints them
on the screen and waits for you to power off. It contains no loader, kernel, exporter or DTB, so it cannot start iPadOS,
and the normal `iOS` entry and its `boot` flag in `kits/SwitchBoard/SwitchBoard.h` are not touched.

`tools/package/test/diag_boot_test.py` limits the script to `setenv`, `test`, `echo`, `md.l`, `sleep` and `true`, and
`md.l` to one aligned word at each address from `native_gate`'s table plus HIDREV and FUSE_RESERVED_ODM4.

## Build

From the repository root, with Python 3.13 and no Bazel:

```sh
PYTHONPATH=tools/package python3 tools/package/diag_boot.py build \
  --template boot/diag/u-boot-diag-script.txt --boards kits/SwitchBoard/SwitchBoard.h \
  --loader-src boot/loader/loader.c --bl31 bazel/pinned/firmware/bl31.bin \
  --bl33 bazel/pinned/firmware/bl33.bin --out build/diag
```

`build/diag` holds `bootloader/ini/ios-diag.ini` and `switchroot/ios-diag/` (`bl31.bin`, `bl33.bin`, and `script.txt`
to read before you copy anything). Copy the two folders onto the SD card's FAT. Nothing existing is overwritten.

## Run

1. Charged battery, console on a table, Joy-Con off or detached. Do not leave it unattended: the fan and the
   temperature sensor are not under our control here.
2. In hekate choose `More configs`, then `iOS Diag`.
3. The screen lists the board (`t210b01`, `sku`) and then one `address: value` line per register. Photograph it.
4. Power off when `DONE` or `REFUSED` shows. The script never exits, and waits with `sleep 1`.
5. Type the lines into a text file and run `diag_boot.py compare LOG --loader-src boot/loader/loader.c` to see each
   value next to the OLED's. That table is not a threshold for the Erista: send the raw lines.

## Stop

- `REFUSED` means the board is not in the table. Nothing was read.
- A screen that stays black or shows `console=FAILED`: hold the power button until the console is off, and say so.
- If the lines stop partway, the next register in `script.txt` is the one that stopped it. Hold the power button until
  the console is off, send the last line shown, and do not retry.
- Never put `ios-diag` files in `switchroot/ios/` or add `ram_oc` keys to its ini.

## On the Erista

Not run on any console yet. Run it only after review.

Artifacts: `build/diag/` as built above, four files: `bootloader/ini/ios-diag.ini`, `switchroot/ios-diag/bl31.bin`,
`bl33.bin` and `script.txt`. The target is `//tools/package:diag_boot`, a `py_binary` with no Bazel build output of its
own. `bazel run //tools/package:diag_boot -- build ... --out /absolute/path` is unchecked, and runs in the runfiles
directory, so every path must be absolute. The Python command above is the one that was run, on Linux.

SD card: copy `bootloader/ini/ios-diag.ini` and `switchroot/ios-diag/` to the FAT partition, keeping the folder names.
Do not touch `switchroot/ios/`. To remove the diagnostic, delete those two items.

The script reads the 27 registers only. DRAM, the framebuffer and the temperature are not read by it, so they are
checks by eye, below. Anything that cannot be seen is reported as not seen, not as fine.

What each run should show

| Item | Seen how | Passes if | If not |
| --- | --- | --- | --- |
| Entry and board | first screen lines | `status=READY`, `console=OK`, and `t210b01` and `sku` are the Erista's | `REFUSED` or `FAILED`: stop, send the lines |
| MC carveout | `compare` on the typed lines | all 25 registers read. A `DIFFERENT` row is a finding, not a failure | `NOT READ` rows: the last line shown is the one that stopped it |
| DRAM | the script itself | it reached `DONE` on the 800 MHz table hekate set: a read of the MC window at all means DRAM trained. Size and timings are not read | a hang before the first line: report it, do not retry |
| Framebuffer | by eye | the console is readable at 720x1280 in the right orientation, no tearing or garbage | note what it shows. Orientation or garbage points at hekate's handoff, not at this script |
| Backlight | by eye | the panel is lit for the whole run | dark with a working console is `console=OK` and nothing visible: power off, note the time it went dark |
| Temperature and fan | by hand | the back is not hot and the fan is heard or felt within the first minute. Nothing here reads a sensor | hot back, or no fan: power off at once. Keep the run under 2 minutes |

Stop at once, by holding the power button until the console is off: a black screen, a stalled line, heat, or any line
the script does not print. Do not retry in the same sitting. The script never leaves its loop and has no path to iPadOS.
