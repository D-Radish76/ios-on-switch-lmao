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
4. Photograph the screen when `DONE`, `INCOMPLETE` or `REFUSED` shows. After 30 seconds the script runs `reset`, which is
   expected to land in hekate (see "What `reset` does"; not seen on hardware). Hold the power button to end it sooner.
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
| Temperature and fan | by hand | the back is not warm. The fan is off on this path, so do not wait for it. Nothing here reads a sensor | a warm back: power off at once. Keep the run under 2 minutes |

Stop at once, by holding the power button until the console is off: a black screen, a stalled line, heat, or any line
the script does not print. Do not retry in the same sitting. The script has no path to iPadOS: its only exit is the
`reset` after the 30 second wait, and a `reset` that does nothing leaves it in a `sleep 1` loop.

## When something is not as expected

The script is not the only code that runs. In this image U-Boot's environment is edited (`DIAG_ENV_EDITS` in
`tools/package/diag_boot.py`; the normal image is not) so that every way out of the script ends on a screen, never in a
boot:

| What goes wrong | What U-Boot does here |
| --- | --- |
| The SD card does not initialize in U-Boot | `mmc dev 1` fails, nothing is scanned, `distro_bootcmd` falls to its end: the screen says `DIAGNOSTIC DID NOT RUN, NO BOOT`, waits 30 seconds and resets once. Not seen on hardware |
| The script fails to run or errors out | `SCRIPT FAILED: continuing...`, then the same end. Boot targets are `mmc1` only, so the eMMC is not tried |
| hekate's environment is not imported | the default `scan_dev_for_extlinux` is replaced by `true` in this image, so no `extlinux.conf` is read from the SD card, whatever the prefix |
| `md.l` fails | the failure is counted out: `INCOMPLETE`, no more reads. A bus fault is not a failure `md.l` returns: it stops U-Boot, and the line count stops there |

The `reset` of `distro_bootcmd` waits 30 seconds in this image and runs once, where stock U-Boot resets after ten seconds.
Where a reset ends is decided by BL31 and hekate, not by this package ("What `reset` does"). A power loss ends in hekate, whose own `autoboot` setting (`hekate_ipl.ini`, not part of this package) decides whether this
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

## Before the first run

Risks that are still open, none of them testable without the console:

- The edited `distro_bootcmd` ends in `sleep 30;reset;` and a `while` loop inside an environment variable. The script uses the same loop and the
  same parser, but this exact line has not run in U-Boot. If it does not parse, the end of a failed run may be a plain
  prompt-less hang, not the message.
- `setenv stdout vidconsole` is what decides `console=OK`. It says U-Boot accepted the device, not that the panel shows
  anything. A lit but empty screen, or a black one, with `console=OK` is possible: stop, and say what was seen.
- If the SD card fails to initialize in U-Boot, the message is printed after `setenv stdout vidconsole`, which needs the
  framebuffer from hekate. If that is not there either, there is no output at all. Treat a black screen with no input as a
  failed run and force the console off (power button held about 12 seconds).
- Temperature and fan: nothing watches or drives them (above). The loop's `sleep 1` is a busy wait on the CPU hekate
  left running, at a clock this project has not read. Keep the run under 2 minutes and stay with the console.
- hekate's `autoboot` and any other entry on the card are outside this package.
- A bus fault in `md.l` stops U-Boot with no message the script controls. The last line on the screen is then the last
  register that worked.

## Path from hekate 6.5.5 to this U-Boot

Read in hekate 6.5.5 (`BLVERSION` 6.5.5 in its Makefile, checked out locally; the 6.5.5 binaries on the card were not
compared with it):

1. Nyx, `More configs`, `iOS Diag` calls `_launch_hos` (`nyx/nyx_gui/frontend/gui.c:903`). It sets autoboot to this entry's
   position, runs `hw_deinit(false)` and jumps into hekate's bootloader.
2. Nyx is built with `BDK_HW_EXTRA_DEINIT`, so that `hw_deinit` runs `fan_set_duty(0)`: the 5 V fan rail is disabled and
   the pin parked (`bdk/thermal/fan.c`), then the TMP451 is shut down (`bdk/soc/hw_init.c:481-490`). **The fan is off from here on.**
3. hekate's bootloader (`bootloader/main.c:741-973`) reads `bootloader/hekate_ipl.ini`, finds the entry from the position
   Nyx set, and calls `launch_l4t`. `Auto Boot OFF` does not matter for this path: Nyx overrides it.
4. `launch_l4t` (`bootloader/l4t/l4t.c:940-1170`) copies every key of the ini section into U-Boot's environment block (at
   `0xA9FC0000`, flagged by the magic `0xA9FBFFFC`), plus `autoboot`, `autoboot_list` and `loader_rev`. It then needs, or
   stops with a red message: Minerva (`Minerva missing`), `switchroot/ios-diag/bl31.bin` and `bl33.bin`, and on the Erista
   `bootloader/sys/l4t/sc7entry.bin`, `bpmpfw.bin` and `sc7exit.bin`. It trains DRAM to 800 MHz, writes no voltage
   (no `ram_oc` keys), sets carveouts, runs `hw_deinit(true)` (display kept, no extra deinit) and starts BPMP-FW, which
   starts BL31, which starts U-Boot at `0xAA000000`.
5. U-Boot's `bootcmd` runs `distro_bootcmd` on `mmc1` only, imports the environment above, and sources the script from its
   slot in `bl33.bin`.

On the card before the run, check that those five files exist and are the ones the Switchroot Linux install on this console
uses. `bpmpfw.bin` is not in this repository and was not inspected.

## Temperature and the hardware's own protection

- Fan: off during the whole run when launched from Nyx (step 2), confirmed in source, not measured.
- Temperature: nothing reads it. hekate's bootloader does not touch the TMP451 after Nyx shuts it down.
- Hardware trip: no hekate code configures `SOC_THERM`. Whether the SoC's own thermal trip is armed, and whether the
  card's `bpmpfw.bin` throttles or powers off on heat, is **not known**. The release's copy has no thermal string (below),
  and BL31 powers the BPMP off on the Erista anyway.
- The diagnostic keeps one CPU core busy in `sleep` for most of the run, with no fan.

## Conditions for the first run

All of these, or no run:

1. The battery is full and the console is unplugged from the charger and the dock. The room is cool, the console lies on a
   hard surface with its vents free, and it is cool to the touch when starting.
2. Someone stays with it for the whole run, a phone camera ready for the screen, and a way to re-launch hekate afterwards
   (the payload injector or whatever started it this time).
3. The five files of step 4 exist, the `ios-diag` files match the hashes taken, and `switchroot/ios/` is not on the card or is
   not launched.
4. Do not touch other entries. No Joy-Con, USB device or dock during the run.
5. Total time from `iOS Diag` to the reset or the power off: under 2 minutes. If the back is warm to the hand at any point, hold the power
   button.
6. One run per sitting. After the run, let it cool for ten minutes and read the photograph before any second run.
7. Stop, and do not retry, on anything in "Stop" above.

Recovery. Hold the power button until the console is off (the documented forced shutdown is about 12 seconds; not tried
here). Nothing in this diagnostic writes to the SD card, the eMMC or the fuses, so the card and the console are as they were:
hekate starts again from whatever injects it. The power button also ends a run in the countdown. If the console is off and
will not start, that is not something this diagnostic is known to cause; say so before trying anything else.

## What `reset` does

Read in the pinned `bl33.bin` (disassembled with `llvm-objdump`) and in Switchroot's ATF source. Not run on any console.

1. U-Boot's `reset` command prints `resetting ...`, waits 50 ms and calls a board function that reads HIDREV
   (`0x70000804`, bits 7:4, the chip major revision; 1 is the Erista, 2 the Mariko). For the Erista it issues PSCI
   `SYSTEM_RESET` (SMC `0x84000009`) and does not return. For the Mariko it takes another path, with a PMIC call and then
   PSCI `SYSTEM_OFF`. U-Boot's own `PMC_CNTRL` main reset sits behind that and is reached only if the PSCI call returns.
2. BL31 is `CTCaer/switch-l4t-atf` (branch `switch-v5-atf2.6`; which commit built the pinned binary is not known).
   Its `tegra_soc_prepare_system_reset` runs `r2p_reboot_to_payload()` when `PMC_SCRATCH0` bit 1 is clear and the chip is
   not a Mariko, and a PMC reset otherwise. hekate sets that bit only when it reboots to RCM on purpose
   (`bdk/utils/util.c:285`), and the BootROM does not.
3. The payload is a copy of hekate's own relocation stub and image that `launch_l4t` puts at `R2P_PAYLOAD_BASE` for the
   Erista, with its boot config cleared (`l4t.c:1106-1109`). So hekate starts again with no launch request and uses
   `autoboot` from `hekate_ipl.ini`.

So the expected landing is hekate, and a loop is possible only if `hekate_ipl.ini` autoboots this same entry. If the pinned
BL31 differs from that source, or the RCM bit was set, the PMC reset sends the console through the BootROM instead, which
boots whatever the eMMC holds. That is not iPadOS and not something this package starts, but it is not hekate either.

`SYSTEM_OFF` is not usable. The same ATF source defines `tegra_soc_prepare_system_off` as an error message and `panic()`,
and U-Boot reaches its `SYSTEM_OFF` call only on the Mariko path. U-Boot has no `poweroff` command. Powering off from the
script would take PMIC writes, which this diagnostic does not do.

## Temperature, fan and CPU load

- The `bpmpfw.bin` of the hekate 6.5.5 release has no thermal, temperature, fan, trip or shutdown string. That does not
  prove it has no thermal code, and BL31 powers the BPMP off on the Erista when hekate hands it an SC7-Entry firmware (read
  in the ATF source earlier, `NOTES.md`), so it is not running during the diagnostic anyway.
- Reading the TMP451 from U-Boot is possible with `i2c md` (read only), but not in this script: the control FDT has an
  alias only for `i2c5` (`i2c@7000d000`), so which U-Boot bus number is the TMP451's `i2c@7000c000` is not known, and
  Nyx put the sensor in shutdown (`tmp451_end`), so what it returns may be a stale reading. Worth adding after a first
  run shows the bus numbers (`i2c dev`), with the sensor's configuration register read next to the temperature.
- Driving the fan means enabling its 5 V rail and programming PWM1 and a pinmux register, which is hekate's
  `fan_set_duty`. That is a sequence of writes, so it stays out of a read only diagnostic.
- U-Boot's `sleep` is a busy wait: the only `wfi` in the whole image is not on the delay path. Every second of waiting
  is one A57 core at full rate. The screen does not need the CPU, since the display scans out of DRAM by itself, but no
  command in this U-Boot parks the core. The two ways would be `go` to a two instruction `wfi` stub placed in the image,
  or the `smc` command with a PSCI call. Both execute code or firmware calls this project has not checked, so neither is
  in. The cost of leaving them out is that the screen is held for 30 seconds, not longer.
- Switchroot's own boot hands over to Linux within seconds, and Linux then runs its thermal and fan drivers (not read
  here). Nothing in that path stays in U-Boot for 30 seconds, let alone minutes.

## The files and the card

What hekate 6.5.5 opens for an Erista launch of this entry (`bootloader/l4t/l4t.c:940-1000`):

| Path on the SD card | SHA256 |
| --- | --- |
| `switchroot/ios-diag/bl31.bin` | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` (pinned) |
| `switchroot/ios-diag/bl33.bin` | the one `diag_boot.py build` wrote. Hash it: it changes with the script |
| `bootloader/sys/l4t/sc7entry.bin` | `676aedce279d0912bb30c1e06c28b6cbfbaee24f7b21cb8e1f7da422b34b28cf` |
| `bootloader/sys/l4t/sc7exit.bin` | `5e9ce4dd187145c096043509aa8e5a576668bdf8c6781d7bc70bece2aaca79a3` |
| `bootloader/sys/l4t/bpmpfw.bin` | `943463950880ae7892a1eb8d3a280b3f86083bca0164c663bf4126210be73ecb` |

And `bootloader/sys/libsys_minerva.bso`, `73d6c05475e530a3e098065024a5a3bbde5a2371123081904c8ac0cb7bae395b`: without it
hekate stops with `Minerva missing`. The last four hashes are of the files in `hekate_ctcaer_6.5.5_Nyx_1.9.5.zip` (its own
SHA256 is `e3c6613fc9e099d00683063855812a6059247da6e9d75f2caa203f9e78e5df56`), downloaded from the project's release page. A
different hash on the card is not an error by itself, but it is a difference to report. Hash the card's copies before the run.
