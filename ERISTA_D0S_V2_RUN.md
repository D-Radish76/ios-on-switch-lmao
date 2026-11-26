# D0-S v2: run sheet

D0-S v2 is not approved to run. This sheet is for the run once it is. One run, no retry, `boot` stays 0. D0-S (v1) is never run
again: it hung (`boot/diag/erista-d0s-run1.txt`).

D0-S v2 is D0's loader plus four read-only device reads, behind a U-Boot script that makes no load from a device address. The
script prints the control FDT's framebuffer properties, as D0-S did, and hands over. The loader reads CAR `RST_DEVICES_L`
(`0x60006004`, 32 bit) and `CLK_OUT_ENB_L` (`0x60006010`, 32 bit), then SDMMC1 Present State (`0x700B0024`, 32 bit) and SDMMC1
Clock Control (`0x700B002C`, 16 bit) only if the CAR words say SDMMC1 has its clock on and is out of reset. It writes no register.
Timeout Control (`0x700B002E`) and Software Reset (`0x700B002F`) are not read. Before each read its BEGIN line is drawn on the screen and the loader
waits 100 ms, so a hang in a read leaves that read's BEGIN line as the last one.

## 1. Build and hash (Mac)

```
bazel test --config=ios17 //tools/package/test:diag_state_v2_test //tools/package/test:diag_state_audit_test \
  //tools/package/test:diag_state_test //boot/diag/loader/test:s2_test //tools/package/test:diag_mmio_audit_test
bazel build --config=ios17 //boot:diag_state_v2 //boot:diag_state //boot:diag_loader_r
PKG="$PWD/bazel-bin/boot/diag_state_v2/package"
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256)
shasum -a 256 bazel-bin/boot/diag_state_v2/loader.uboot.bin bazel-bin/boot/diag_state/loader.uboot.bin \
  bazel-bin/boot/diag_loader_r/loader.uboot.bin
python3 tools/package/diag_mmio_audit.py --disassembly bazel-bin/boot/diag_state_v2/loader.dis \
  --uboot-image bazel-bin/boot/diag_state_v2/loader.uboot.bin --s2
```

The Mac build of 2026-11-26 of the first v2 (32 bit Clock Control word) gave `9c41874e...`; the 16 bit build has another hash. Compare
only builds of the same commit.

The package has four files: `bootloader/ini/ios-diag-state-v2.ini` and `switchroot/ios-diag-state-v2/{bl31.bin,bl33.bin,script.txt}`.
D0-S's `ios-diag-state` files and the other entries are not written by this build. `bl31.bin` equals the other diagnostics'
(`dea0ecd4...34e2b4`).

The audit must end with `AUDIT PASSED (D0-S v2): the image holds exactly 0x60006004, 0x60006010, 0x700b0024 and 0x700b002c, each
once ...`. That line is printed only by `--s2` and only after the D0-S v2 inspection of the disassembly (one `ldr w0` word load, one
`ldrh w0` halfword load, each called only by `switch_s2_access`, no store on an address path) and of the flat image's address words
passed; the generic `only the two identity registers` line is not used for it. Section 6 reads `register address words match the D0-S
v2 list` and section 3 names `switch_s2_read32: ldr w0` and `switch_s2_read16: ldrh w0`. A saved Linux copy is `boot/diag/erista-s2-mmio-audit.txt`; the Mac addresses differ.

This loader is not D0-R's: it has the S2 reads and no reservation check, so `loader.uboot.bin` differs from D0-R's and D0-S's. The
three lines above make that visible. D0-S's and D0-R's hashes stay what the earlier Mac builds gave (`cdc52c65...`). The Linux flat
image hash, comparable only with another build from Ubuntu clang 18.1.3, is the `diag_state_v2` line of
`boot/diag/erista-diag-image-hashes.txt`. Record the four package hashes in the review before copying.

## 2. Copy and check (Mac)

`cp -n` refuses to replace a file. If it prints nothing and the check below fails, a file of that name already existed.

```
export COPYFILE_DISABLE=1
SD="/Volumes/CARD"
ls "$SD/switchroot/ios-diag-state-v2" "$SD/bootloader/ini/ios-diag-state-v2.ini" 2>&1
mkdir -p "$SD/switchroot/ios-diag-state-v2"
cp -n "$PKG"/switchroot/ios-diag-state-v2/bl31.bin "$PKG"/switchroot/ios-diag-state-v2/bl33.bin \
  "$PKG"/switchroot/ios-diag-state-v2/script.txt "$SD/switchroot/ios-diag-state-v2/"
cp -n "$PKG"/bootloader/ini/ios-diag-state-v2.ini "$SD/bootloader/ini/"
sync
(cd "$PKG" && find . -type f | sort | xargs shasum -a 256) > /tmp/d0sv2.sums
(cd "$SD" && sed 's|  \./|  |' /tmp/d0sv2.sums | shasum -a 256 -c -)
find "$SD" -name '._*' -path '*ios-diag-state-v2*'
```

The first `ls` should say `No such file`. All four lines of the check print `OK` and `find` prints nothing. Eject, insert again and
check a second time. Any `FAILED`, missing file or `._` file: do not run.

## 3. Before starting

| Check | |
| --- | --- |
| Experiment card only, backed up | the `iOS`, `iOS Diag`, `iOS Diag State` and older diagnostic entries stay as they are |
| hekate 6.5.5, Auto Boot off | nothing in `bootloader/ini` starts on its own |
| Temperature | cold start: off for 10 s, then hekate. Read Nyx's temperature. Start only below 40 C |
| Battery, placement | charged, charger out, hard surface, vents clear, no case, Joy-Con off |
| Camera | recording from before `More configs` until hekate is back; a second photograph of the final screen |
| Someone at the console | within reach of POWER |

The 40, 50 and 55 C figures and every deadline below are provisional. No limit for this console is known.

## 4. The run

Launch `iOS Diag State V2` from Nyx "More configs". Film the screen from the first U-Boot line.

U-Boot lines, in order (the same as D0-S's up to the framebuffer line):

```
D0-S V2 STATE DIAGNOSTIC. READ ONLY. NO KERNEL. NO BOOT
BOARD OK
FRAMEBUFFER OK
DTB COPIED AND FRAMEBUFFER RESERVED
LOADER WRITTEN TO 0xA0000000
D0-S V2 FRAMEBUFFER PROPERTIES: ADDRESS f5a00000, SIZE PROPERTY LENGTH 4, 2d0 X 500, a8b8g8r8, ROT 3
HANDOFF TO THE LOADER. THE SCREEN CLEARS, THEN THE REPORT
```

D0-S printed the first six lines too. Loader size changes (the S2 files are in it); the framebuffer line equals
`boot/diag/erista-d0s-run1.txt`.

Then the screen clears and the loader draws the D0 header (`LOAD DIAG S2 ...`), the EL1 and board lines, and one line per step from
row 9 down, each one drawn, followed by a 100 ms wait, before the next step begins:

```
S2 START. EL1 STATE PASS
S2 CAR RST READ BEGIN. ADDRESS 60006004. 32 BIT
S2 CAR RST READ RESULT <8 hex digits>
S2 CAR CLK READ BEGIN. ADDRESS 60006010. 32 BIT
S2 CAR CLK READ RESULT <8 hex digits>
S2 SDMMC1 CLOCK/RESET GATE. CLOCK ON. RESET OFF. READ        (or CLOCK OFF. NOT READ / IN RESET. NOT READ)
S2 SDMMC1 PRESENT STATE READ BEGIN. ADDRESS 700B0024. 32 BIT
S2 SDMMC1 PRESENT STATE READ RESULT <8 hex digits>
S2 SDMMC1 CLOCK CONTROL READ BEGIN. ADDRESS 700B002C. 16 BIT
S2 SDMMC1 CLOCK CONTROL READ RESULT <4 hex digits>
S2 COMPLETE. READS 4. STOP 0000
```

Then `BOOT STAYS 0`, the 15 s countdown and the PSCI reset to hekate. When the gate says NOT READ, the four SDMMC1 lines are absent,
the complete line says `READS 2` and that is a valid result.

The CAR words T1-T read under the same hekate were `049102CA` and `9802D131` (`boot/diag/erista-t1t-run1.txt`), which open the gate.
A different value is a finding, not a failure.

## 5. Reading the last line of a hang

| Last line on the screen | The hang is in |
| --- | --- |
| `D0-S V2 FRAMEBUFFER PROPERTIES ...` or `HANDOFF TO THE LOADER` | U-Boot or the handoff. v2 has no device load in U-Boot, so this would not be D0-S's cause |
| the loader header, no `S2 START` | the EL1 and board lines before S2 |
| `S2 CAR RST READ BEGIN` | the read of `0x60006004` |
| `S2 CAR CLK READ BEGIN` | the read of `0x60006010` |
| `S2 SDMMC1 PRESENT STATE READ BEGIN` | the read of `0x700B0024` |
| `S2 SDMMC1 CLOCK CONTROL READ BEGIN` | the 16 bit read of `0x700B002C` |
| a RESULT line | the code after that read, not the read |

The loader draws a line, issues `dsb sy` and waits 100 ms before the read. The CPU cannot observe the display's scan-out, so this
is a margin, not a proof: if the last line is a BEGIN line, the read is the suspect; a missing BEGIN line does not clear it.

## 6. End conditions and recovery

The run ends by itself: the loader's report is drawn, then 15 s later PSCI `SYSTEM_RESET` returns to hekate. Total about 30 s from the
first line. Return to hekate is expected, as in the D0 and D0-R runs.

| What you see | Do |
| --- | --- |
| Report, then hekate's menu after about 15 s | Done. |
| The screen stops and never advances | Treat as a hang. Hold POWER until the console is off. Photograph first. Report the last line. |
| Nothing after the entry is selected for 20 s | Hold POWER. Report. |
| `S2 COMPLETE ... STOP 0801` or `0802`, `PSCI SYSTEM_RESET RETURNED`, `NO RESET`, `NO EL CHANGE`, `EXIT CALLED OUTSIDE EL1` | Hold POWER. |
| The console feels hot or the screen shows garbage | Hold POWER at once. |
| Any deadline in `boot/diag/loader/RUN_CHECKLIST.md` passes | Hold POWER. Do not retry on the spot. |

Stop `0801` is a read the allowlist refused, by register or by width (a loader bug, nothing was read); `0802` is a line that did not settle (the counter or the
screen was not usable), and no further read was made. Recovery is the held power button, then hekate as usual. There is no other
stop: a hang has no hardware reset (`ERISTA_STOP_PATHS.md`).

### U-Boot failure codes

U-Boot prints `REFUSED: <status>`, waits 15 s and runs `reset`. Nothing is booted. The statuses and what each left in RAM are D0-S's
(`ERISTA_D0S_RUN.md` section 5): `WRONG_BOARD`, `CONSOLE_FAILED`, `CONTROL_FDT_FAILED`, `FRAMEBUFFER_PROPERTY_FAILED`,
`FRAMEBUFFER_MISMATCH`, `DTB_COPY_FAILED`, `DTB_HEADER_FAILED`, `DTB_PADDING_FAILED`, `FRAMEBUFFER_RESERVE_FAILED`,
`BOOT_ENVIRONMENT_FAILED`, `BOOT_HEADER_FAILED`, `IMAGE_LOAD_FAILED`, `BOOT_PREP_FAILED`, `UNEXPECTED_BOOTM_RETURN`. A status not in the
list: assume everything above was written. A reset or power-off discards the RAM.

## 7. What the SDMMC1 reads are

Present State is one 32 bit aligned load (`ldr w`). Clock Control is one 16 bit halfword load (`ldrh w`), the width the register has
in the SD host controller layout. Both run at EL1 with the MMU off (Device memory). The loader's gate refuses a width other than the
listed one, so `0x700B002C` cannot be read as 32 bit and `0x700B0024` cannot be read as 16 bit.

| Address | Register | Width | Read side effect |
| --- | --- | --- | --- |
| `0x700B0024` | Present State, read-only status | 32 bit | none in the standard; it reports the card, the data lines and the buffer flags |
| `0x700B002C` | Clock Control (internal clock enable and stable, card clock enable, divider) | 16 bit | none in the standard |

Not read: Timeout Control (`0x700B002E`, 8 bit), Software Reset (`0x700B002F`, 8 bit) and the buffer data port (`0x700B0020`, whose
read pops the buffer). None of the three is on the allowlist, and the inspection fails the image if one of those addresses appears as
data. The 32 bit read of `0x700B002C` that the first v2 build made was dropped for lack of evidence.

Sources for the layout and the widths:

- hekate `bdk/storage/sdmmc_t210.h`: `0x24 prnsts` is `vu32`, `0x2C clkcon` is `vu16`, `0x2E timeoutcon` and `0x2F swrst` are `vu8`;
  `bdk/storage/sdmmc_driver.c` reads and updates `clkcon` and `swrst` as those members and flushes writes with a read of `clkcon`.
- Linux `drivers/mmc/host/sdhci-tegra.c`: reads go through `readw(ioaddr + reg)` and `sdhci_readw(host, SDHCI_CLOCK_CONTROL)`;
  nothing there reads `0x2C` as a 32 bit word.

Neither source states the width in words, hekate's from the member types. The Tegra TRM is not in the repository. **No read of SDMMC1
by this loader has run at EL1 yet**: hekate and Linux read these registers in other settings (hekate bare metal with its own clock
setup, Linux with the controller's driver), and T1-T never read SDMMC1. That the controller answers these two reads with the clock on,
at EL1, with this loader, is what the run would show. A read of a controller whose clock is off can hang the bus; the gate reads CAR
first for that reason.

## 8. Temperature

The exposure is U-Boot's few seconds, the 5 s, up to 1.1 s of settle waits (11 lines at 100 ms; 7 lines, about 0.7 s, when the SDMMC1 gate is closed) and the loader's 15 s with the fan off.
Nothing in D0-S v2 manages the fan or reads a sensor. After the run: photograph first, then Nyx's temperature. Do not start another
run until the console is below 40 C again. Above 50 C at the end: leave it off. Above 55 C: stop the session and report. All three are
provisional.

## 9. Effect on existing files

| Where | Change |
| --- | --- |
| SD card | two additions: `bootloader/ini/ios-diag-state-v2.ini` and `switchroot/ios-diag-state-v2/` |
| D0-S, other entries, `iOS` | untouched. `boot` 0 means the iOS entry cannot start either way |
| RAM | the D0 writes: the DTB copy at `0x8D000000`, one reserve-map entry, the loader at `0xA0000000` |
| U-Boot variables | set, never `saveenv`, gone at reset |
| Console flash, eMMC | not touched |
| Registers | four reads (three 32 bit, one 16 bit), no write |

After the run: remove `switchroot/ios-diag-state-v2/` and the `.ini`, then re-run the `find` for `._` files. D1-min, T2 and the first
XNU boot stay held.
