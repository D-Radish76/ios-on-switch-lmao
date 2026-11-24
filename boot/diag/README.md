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
