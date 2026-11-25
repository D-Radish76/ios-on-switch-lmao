# D1 design and the temperature plan

For review. Nothing here is implemented. `boot` stays 0 for the Erista; D2, XNU, voltage changes and an EMC raise are not
part of it.

## What D0 settled and what it did not

Settled by the run (`boot/diag/erista-d0-run.log`): the loader reaches EL1 on the Erista, reads 25 MC rows equal to the
table, reads the GIC, walks the DTB without finding an overlap and resets to hekate through PSCI from EL1.

Not settled, and D1 inherits it: thermal protection, memory no list names, the hekate DTB's size, the origin of one of the
three reserve-map entries, whether the exporter's view of U-Boot's objects holds on this console.

## D1 as defined

`ERISTA_LOADER_DIAG.md`: the exporter and the SD reference validate, read only, on the Erista's card. In the normal path
(`boot/u-boot-script.txt`) this is three steps after `bootm prep`:

1. The exporter (`exporter.uimg`, pinned) is loaded to `0xA7000000` by `bootm loados`; two U-Boot helpers at
   `0xAA001920` and `0xAA001A64` make it visible to instruction fetch.
2. U-Boot reads sector 0 of `mmc 1` to `0xA001F200` (a card read through U-Boot's driver).
3. The exporter, run with `go 0xA7000000`, reads U-Boot's cached card objects from RAM and writes a 256 byte record at
   `0xA001F400`. It issues no card command and touches no MMIO.

The loader then validates the record with `boot/storage/sd_reference.c`.

## Why it carries over to the Erista, and where it may not

Carries over, from what was measured:

- The exporter is built for exactly the pinned `bl33.bin` (`b492a401...`). The Erista runs that file.
- It requires the relocated image inside `0xF0000000..0xFFD00000` and a heap at `[relocaddr - 0x402000, relocaddr)`.
  Measured: `relocaddr 0xFFC87000`, so the heap is `0xFF885000..0xFFC87000`, and `sp start`, `irq_sp` and `fdt_blob` sit
  just below it (`0xFF87F710`, `0xFF87F728`). This supports the layout assumption. It does not test the object offsets.
- It requires the SD controller at `0x700B0000` and block device 1. The pinned U-Boot's `boot_targets` is `mmc1`, and
  hekate loaded `bl33.bin` from that card.

May not, and is only found by running:

- The card's mode. The exporter refuses anything but SD version, OCR ready with CCS, CSD version 2, 4 bit SDR and 512 byte
  blocks. A 32 GB card satisfies CSD version 2; the bus mode on the Erista is not known.
- `${devnum}` equal to `1` in the script, a test the normal script makes and the D0 script does not.
- The memory around `0xA7000000` (64 KiB of exporter and its 16 KiB stack) and `0xA001F000..0xA0020000` beyond the
  loader's range. Neither is in any list of occupied memory, and neither is proven free.

## Steps, each reviewed before the next

**D1-pre: display only, U-Boot only, no loader.** A hekate entry like `iOS Diag Memory`. It prints `devnum`,
`mmc list` and `mmc info` for device 1, and runs `reset`. It writes nothing to RAM beyond U-Boot's own state. It answers the
card mode and `devnum` questions before any exporter code runs. The command list is an allowlist with a test, as for D0-pre.

**D1: the D0 image plus the exporter and the SD reference.** Changes against D0:

- The script template takes the exporter, SD reference and record steps from `boot/u-boot-script.txt` (the same lines, so
  the Erista runs what the OLED runs). The status names stay distinct from D0's.
- The loader prints the record's fields (CID, CSD, RCA, capacity, bus width) and the result of `sd_reference.c`'s
  validation. It adds one thing D0 lacks: every reserve-map entry's base and size, to settle the third entry.
- No write to the card, and no access by the loader to `0x700B0000`.
- `check_destination` also tests `0xA7000000..0xA7010000` and `0xA001F000..0xA0020000` against the DTB's reservations and the
  measured layout (`memory_layout.regions`).
- The link inspection of the loader stays as for D0 (no new CPU register write). A new rule: no load or store to
  `0x700B0000`, checked from the disassembly.

**What D1 leaves out:** `native_gate`, `boot 1`, the staged window and the kernel. The `mc_verified` step from the roadmap
(item 3) is a separate change and a separate review: D0 now satisfies its condition, and it still does not lift `boot`.

## Verification plan

Host, before any hardware run (Linux and Mac):

1. `d0_run_test`, `diag_loader_test`, `memory_layout_test`, `hush_lint_test` as now.
2. The exporter's existing `test_export.py` and QEMU `model.py`, unchanged: they pin the exporter against the pinned
   `bl33.bin`.
3. New: a test that the D1 script's command list is the OLED script's exporter and SD reference lines and nothing else (no
   `saveenv`, no `mmc write`, no `mw` outside the two documented ranges).
4. New: an Erista card fixture for `sd_reference.c`, typed from the D1 run, so a later change cannot break the check
   unseen.
5. Build, link inspection, hash comparison on the Mac, then `RUN_CHECKLIST.md` rewritten for D1.

Hardware: one attended run with the D0 rules (60 s limit, power button, filmed), after D1-pre has shown a card in the mode
the exporter accepts. Success is the loader's printed record equal to what D1-pre's `mmc info` showed. Any refusal is
reported and stops the work.

## Temperature

### What is known

- A launch from Nyx `More configs` runs `hw_deinit`, which stops the fan (PWM1 duty 0) and puts the TMP451 in shutdown.
  In shutdown its registers keep the last value, so a read after Nyx shows the temperature when Nyx left, not the current
  one (`ERISTA_RESEARCH.md`, `SwitchThermal.cpp`).
- Nothing in hekate's bootloader build programs SOC_THERM's trips. BL31 turns the BPMP off, and the release's `bpmpfw.bin`
  has no thermal strings. Whether the hardware's reset-time thermal trip is armed on an Erista is not known.
- The drivers in this repository (`SwitchSocTherm.cpp`, `SwitchThermal.cpp`) are the T210B01 path: PLLX thermtrip at
  96 C, CPU and GPU at 127 C, a PMC reset on the trip, the fan on PWM1 (`0x7000A010`) through pin LCD_GPIO2 with the 5 V
  regulator on GPIO PA5. Their sensor set and calibration are for the OLED. hekate's `fan.c` and `regulator_5v.c` have
  separate T210 and T210B01 branches; only the second was read. Do not reuse any of it on the Erista before the first
  branch is read.
- The pinned U-Boot has `i2c bus`, `i2c dev`, `i2c md`, `i2c probe`, `gpio` and `pwm` commands (strings in `bl33.bin`).
  The TMP451's U-Boot bus number is not known.

### What the D0 and D1 runs expose

Each is under a minute, with the fan off, with a person present. The earlier diagnostic runs (the first diagnostic, the
memory pages, D0) were each 15 to 25 s. That says what happened, not that it was safe.

### Plan, in order, each a separate review

**T1: read the TMP451 without writing to it.** A display-only page in the D0-pre style:
`i2c bus`, `i2c dev <bus>`, then `i2c md 4c 00 2` (local and remote degrees), `i2c md 4c 03 1` (config, bit 6 shows
shutdown) and `i2c md 4c fe 1` (manufacturer ID `0x55`). No `i2c probe` (it writes to every address on the bus) and no
`mw`, `mm`, `nm`. A pointer write that selects the register is part of every I2C read. Expected on a Nyx launch: shutdown
set and a stale value. That is still useful: it is the temperature when D0 began, and it confirms the bus and address.
Needs the bus number first: the first version prints `i2c bus` only.

**T2: a live reading.** Clearing the shutdown bit (config register write) or a one-shot conversion is a write to the sensor.
Small and the sensor's own, but a write on a bus shared with other devices: needs its own review and the bus identified by
T1.

**T3: fan.** A write sequence (clock, reset, pinmux, regulator, PWM) read from hekate's Erista branch, then a bounded test:
fan on for 10 s, a person listening and looking. Not before T1 and T2, and not part of D1.

**T4: a bound for any run longer than seconds:** a hardware trip or a watchdog. The SOC_THERM trip registers and the PMC
reset bit are the candidates; their T210 reset values and calibration have to be read from the TRM or L4T's `soctherm.c`
for T210 first. Until then the bound is a person at the console, the 60 s rule and a cool, hard surface.

### Rule until T1 to T3 are done

No run longer than D0's. No kernel. A thermal reading is never a reason to relax an abort condition.

## Questions for the reviewer

1. Is D1-pre worth its own run, or should its commands be the first lines of D1's U-Boot half? Separate is cheaper to
   abort; combined is one fewer card read.
2. T1's stale reading: accepted as useful, or should T1 wait for T2?
3. Should the reserve-map print and the `mc_verified` change travel with D1 or alone?
