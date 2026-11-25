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
   `DONE: all 27 registers read` means every `md.l` returned success and was counted. `INCOMPLETE` names the register that
   failed (`status=READ_FAILED_<address>`) and the count so far, in hex.
4. Power off when `DONE`, `INCOMPLETE` or `REFUSED` shows. The script never exits, and waits with `sleep 1`.
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
| MC carveout | `DONE: all 27 registers read`, then `compare` on the typed lines | all 25 registers read. A `DIFFERENT` row is a finding, not a failure | `NOT READ` rows: the last line shown is the one that stopped it |
| DRAM | not checked by this script | nothing here tests DRAM. `DONE` says the memory controller's registers answered and U-Boot, which runs in DRAM, is running. Size, timings, stability and the carveout contents are not read or tested | a hang before the first line: report it, do not retry |
| Framebuffer | by eye | the console is readable at 720x1280 in the right orientation, no tearing or garbage | note what it shows. Orientation or garbage points at hekate's handoff, not at this script |
| Backlight | by eye | the panel is lit for the whole run | dark with a working console is `console=OK` and nothing visible: power off, note the time it went dark |
| Temperature and fan | by hand | the back is not hot and the fan is heard or felt within the first minute. Nothing here reads a sensor | hot back, or no fan: power off at once. Keep the run under 2 minutes |

Stop at once, by holding the power button until the console is off: a black screen, a stalled line, heat, or any line
the script does not print. Do not retry in the same sitting. The script never leaves its loop and has no path to iPadOS.

## When something is not as expected

The script is not the only code that runs. In this image U-Boot's environment is edited (`DIAG_ENV_EDITS` in
`tools/package/diag_boot.py`; the normal image is not) so that every way out of the script ends on a screen, never in a
boot:

| What goes wrong | What U-Boot does here |
| --- | --- |
| The SD card does not initialize in U-Boot | `mmc dev 1` fails, nothing is scanned, `distro_bootcmd` falls to its end: the screen says `DIAGNOSTIC DID NOT RUN, NO BOOT` and it waits. Not seen on hardware |
| The script fails to run or errors out | `SCRIPT FAILED: continuing...`, then the same end. Boot targets are `mmc1` only, so the eMMC is not tried |
| hekate's environment is not imported | the default `scan_dev_for_extlinux` is replaced by `true` in this image, so no `extlinux.conf` is read from the SD card, whatever the prefix |
| `md.l` fails | the failure is counted out: `INCOMPLETE`, no more reads. A bus fault is not a failure `md.l` returns: it stops U-Boot, and the line count stops there |

The `reset` at the end of `distro_bootcmd` is removed in this image only. A reset from anything else, or a power loss,
ends in hekate, whose own `autoboot` setting (`hekate_ipl.ini`, not part of this package) decides whether this
entry starts again. Set it to 0 on the card you use.

## Temperature and fan

What the code shows (hekate at `8e1392d`, checked out locally; no console run):

- The `bootloader/` part of hekate never calls `fan_set_duty`, `tmp451_init` or any `soc_therm` register. Only
  `hw_deinit` does under `BDK_HW_EXTRA_DEINIT` (`nyx/Makefile` defines it) and Nyx's GUI reads the TMP451. So what the
  fan is doing at the handoff is what hekate or Nyx left, and nothing in this diagnostic changes it.
- No code in hekate configures `SOC_THERM` trip points. Whether a hardware thermal trip is armed at that moment was not
  found in any source here.
- In this diagnostic the CPU runs U-Boot's `sleep 1` loop. The clock and voltage hekate left are not read.

So nothing watches or controls temperature during a diagnostic run, which is why the run is by hand and short. The
same holds for Switchroot Linux until its own kernel's fan and thermal zones start, so a stock Switchroot boot
guarantees nothing about the temperature before that point either. `ERISTA_RESEARCH.md`, section 11, has the rest.
