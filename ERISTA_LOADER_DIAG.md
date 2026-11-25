# Erista: from the first diagnostic to the first XNU start

Facts from the first run are in `ERISTA_RESEARCH.md`, section 12, and `boot/diag/erista-first-run.log`. Nothing here is built
on the Mac or run on a console. `boot` stays 0 for the Erista throughout.

## What the first run measured

The 25 memory controller registers `native_gate` compares were all equal to its OLED values, read by U-Boot's script
(`md.l`) at the start of its script. HIDREV `0x00022117` is a T210 (chip `0x21`, major 1) and FUSE_RESERVED_ODM4
`0x00000204` has type 0, so the board is `t210b01=0`, `sku=0`. `native_gate` writes nothing, so equal values mean no MC
register needs rewriting.

## Is the loader's MC the same as the diagnostic's

Close to certain, not proven. What the sources show, in time order:

1. hekate sets the carveouts once in `launch_l4t` (`_l4t_mc_config_carveout`, `bootloader/l4t/l4t.c:487`). It takes only the
   chip type. No ini key reaches it, so the `iOS` and `iOS Diag` entries run the same code (read in hekate 6.5.5).
2. BL31 runs next, then U-Boot. BL31's source was not read for MC writes. Both entries go through it before U-Boot.
3. U-Boot's board init writes the video protect registers once (`0x7001964c` to 0, `0x70019650` to 1; the function at
   image offset `0x3390` of the pinned `bl33.bin`, called from the init function at `0x2724`). It runs before any script,
   so the diagnostic's reading already includes it.
4. The rest of U-Boot's code that addresses the MC block (found by its `#0x7001` immediates, six sites) reads: the
   registers it prints into the FDT's reservations are in `native_gate`'s table. No other store was found. A write through
   a pointer taken from the device tree would not show in that search.
5. Between U-Boot's script and `native_gate` the loader's path runs `bootm`, SD reads, the exporter and the loader's own EL
   change. The loader (`boot/loader/*.c`) never writes the MC block: its only MC addresses are the reads in `native_gate`. Its
   EMC step writes BPMP channels and atomics (`0x70016000`) and is off for the Erista (no `EMC` feature).

What is left open: BL31's own SMC services at run time, a store through a device-tree pointer, and the time difference
(seconds). The proof is the loader reading the same 25 registers at `native_gate`'s moment, which is the loader diagnostic
below, and it needs no change to `boot`.

## Every condition on the way to the kernel

Status for the Erista: **seen** (the first run exercised it), **same** (same code or silicon, a reason given, not run),
**open** (not exercised, no reason to expect either), **no** (written for the OLED, wrong or unusable on an Erista).

| Where | Condition | Status |
| --- | --- | --- |
| script | `stdout vidconsole` works | seen: `console=OK` |
| script | board table row, `t210b01`/`sku` | seen: `0`/`0`, row `erista`, `boot 0`, so the gate says no by design |
| script | U-Boot code words at `0xAA001920`..`1A6C` | same: the diagnostic image carries the same U-Boot code, only the environment text differs |
| script | `/framebuffer` in the control FDT: `0xF5A00000`, 720x1280, `a8b8g8r8`, rot 3 | open: the console drew, so a framebuffer exists; the properties were not read |
| script | `bootargs.txt` by `fatsize` and `load` from partition 1 | open: only `mmc dev 1` and the env import ran |
| script | DTB copy, kernel window header and trailer, `fdt addr` and `rsvmem`, exporter `bootm loados`, `go` helpers | open: the diagnostic image has none of them |
| script | `bootm start`, `loados`, `prep` for the loader | open |
| script | SD reference: `read mmc 1`, exporter `go 0xA7000000` | open: needs the exporter running with the Erista's card |
| loader | EL1, SCTLR, DAIF, CNTFRQ 19.2 MHz, HCR, CNTHCTL (0x301) | same: same cores, same BL31, but the transition has not run |
| loader | `board->boot` (0x306) | no, by design: this is the gate that stays closed |
| loader | EMC raise through the BPMP | no: BL31 powers the BPMP off on the Erista, and the VDD_CORE check is a T210B01 table. Off by the board's features. DRAM stays at 204 MHz (below) |
| loader | live FDT reservations against the loader's ranges (0x500, 0x600, 0x680, 0x6c0, 0x740) | open: the FDT is U-Boot's, built from the equal carveouts, so it should match |
| loader | staged window header and trailer (0x6a0..0x6a3) | same: the window is build input, not board input |
| loader | `native_gate` board name is `oled` (0x402) | no: the rule names the OLED. The data now says the Erista's table is the same |
| loader | entry stack between `0xf0001000` and `0xffd00000` (0x401) | same: U-Boot relocates below the same top of RAM, since the carveout registers are equal. Not read |
| loader | 25 MC registers (0x410+i) | seen at U-Boot's moment, open at the loader's |
| loader | GIC distributor and CPU interface reads | open: same SoC addresses, observation only |
| loader | ISAR0 CRC32 field is 1 (0x305) | same: the OLED and the Erista both have A57 cores |
| loader | SD reference validate (0x640+) | open |
| loader | copy and CRC of the window, argument checks (0x304), board word (0x307), bootargs.txt (0x760+) | same: board independent, host tested |
| loader | `LOAD_FULL_RAM`: SDHCI `0x700b0024` idle, two memory banks (0x710) | open: the bank count comes from the Erista's FDT |
| loader | SE2 seed | no: SE2 is T210B01 only. Off by the features, the pinned seed is kept |

## Loader diagnostic, before any kernel

Goal: run the normal loader's path on the Erista up to `native_gate`'s moment, print what it sees and stop. It does not lift
`boot`, carries no kernel and cannot start XNU.

D0 is implemented in `boot/diag/loader/` (`README.md` there has the CPU state it changes and the inspection). It differs from
the first sketch in one way: it is not a `LOAD_DIAG` build of `loader.c`. It is its own set of sources that reuses
`memory.c`, `handoff/entry.S`, `display.h` and `record.h` unchanged, so no `#if` in the normal loader decides whether a kernel
can start. `bazel build //boot:diag_loader` gives its hekate entry (`iOS Diag Loader`), its `bl33.bin` (U-Boot, script, loader,
Erista DTB; no exporter, no window) and the map, disassembly and inspection the link had to pass.

What it prints: the board row, HIDREV and ODM4, the EL1 state words, ISAR0, the entry stack against `native_gate`'s range, the 25
MC registers against the OLED values (`SAME` or `DIFFERENT` with the expected value), the six GIC words, the normal loader's FDT
memory walk over the loader image, SD reference page, staged window capacity, kernel destination and boot-args page. Then 15 s
on the CPU counter and PSCI `SYSTEM_RESET`.

Preconditions are checked before the work that depends on them. FDT pointer, header, reservation block and framebuffer (inside
DRAM, clear of the image, covered by an FDT reservation) are checked at EL2, before the EL change: if one fails the loader parks
there, draws nothing and never changes EL; U-Boot's own progress lines are then the only thing on screen. After the EL change, a
failed EL1 state skips every device read, HIDREV and ODM4 included; an unknown board skips the MC and GIC reads; a failed FDT check
skips the memory walk. The record words 48 to 54 in `probe_record` keep the codes for a debugger. `bootm loados` writes
`0xA0000000` before any loader code runs; only the build can check that span (`check_destination`, now including the DTB's reservations), see below and the README.

Hardware writes: framebuffer RAM, the loader's own RAM, and the CPU system registers listed in `boot/diag/loader/README.md`.
None to a peripheral and none to the MC block.

Unverified, and treated so: PSCI `SYSTEM_RESET` from EL1 in the loader (U-Boot's call was from EL2), and the generic timer's event
stream waking `wfe`. If the counter's frequency is not exactly 19.2 MHz or does not advance, the loader parks instead of resetting.

Stages, each reviewed before the next:

1. `D0`: everything above, no SD, no exporter.
2. `D1`: the exporter and the SD reference validate, read only (the Erista's card).
3. `D2`: the staged window's header, trailer and CRC, with the copy left out.

## What is still open, by area

| Area | Known | Open |
| --- | --- | --- |
| Memory layout | MC carveouts equal the OLED's. The loader's destination, boot args and stamp slots are board independent | the Erista's FDT banks and reservations, the entry SP, whether `0xA0000000` and `0xF5A00000` are free as on the OLED |
| DRAM speed | hekate trains 800 MHz and then `hw_deinit` runs `minerva_deinit`, back to 204 MHz (`bdk/mem/minerva.c:152`). `emc.h` says the same | the loader and the kernel both run at 204 MHz on an Erista: raising it needs the BPMP, which BL31 turns off. Needs its own EMC code, with Minerva's tables, and voltage review. Not started |
| BL31 ABI | PSCI `SYSTEM_RESET` works (observed). `SYSTEM_OFF` panics in the ATF source. The BPMP is off after BL31 on an Erista | SMC services the kernel uses (CPU on, suspend), SC7 on an Erista, whether the pinned binary is the source commit |
| ADT | `generate_adt(board=)` and the board word exist. `ERISTA_RESEARCH.md` section 2 lists the OLED nodes left | an ADT without the OLED panel, touch and MAX77812 nodes, from the Erista DTB |
| Framebuffer | hekate's handoff drew the console on the Erista's panel | rotation and format properties; the backlight stays on hekate's setting (no PWM back end), the display driver gate `sc7Display` still panics for a non-OLED panel |
| SD | U-Boot reads the Erista's card. The kernel's SD driver skips the 1.8 V switch on a T210 | the exporter and the SD reference on the Erista; UHS modes; the card reset and suspend paths write voltage |
| PMIC | MAX77620 is on both boards | CPU rail is MAX77621 at I2C5 `0x1B`, the OLED's MAX77812 code returns on a failed read before writing (not run); charger limits for the HAC-001 battery |
| Temperature | the TMP451 is on I2C1 (`0x7000c000`, device `0x4c`). The pinned U-Boot's control FDT disables that controller and the PWM block: its only I2C bus is I2C5 (the PMIC), so U-Boot cannot read the TMP451 or drive the fan as the image stands (`ERISTA_THERMAL.md`). Nothing reads or drives the fan or the TMP451. The kernel's watchdog service touches no hardware (`ERISTA_RESEARCH.md`, section 5), so a hung kernel is not bounded by anything | how to read the TMP451 at all; the Erista branches of hekate's fan and 5 V code; a fan and temperature plan before a kernel runs for more than seconds |

## Order of work to the first XNU start

1. Record the first run (done) and keep `boot 0`.
2. The loader diagnostic, D0 then D1 and D2. It answers the open rows above in the loader's own context.
3. Make `native_gate` data driven: the Erista row carries `mc_verified`, set only after D0 shows 25 equal at the loader's
   moment. The name test goes, the table stays the OLED's, and `boot` stays 0. A host test: an Erista row without the mark
   still refuses.
4. An ADT for the Erista (no OLED panel, touch or MAX77812 nodes) and the kernel's board thread at `features 0`, which is
   already the minimal set.
5. A temperature and fan plan, read only first: the TMP451 on its U-Boot bus, shown in the diagnostic. Then a decision on
   the fan, which is a write sequence from hekate and needs its own review. A bound on the first kernel run: a hardware
   watchdog, or a person at the console with the power button.
6. DRAM at 204 MHz is acceptable for a first start that only has to reach a console, so EMC work comes after it.
7. `boot 1` for the Erista, in its own commit with its own review, after D2 and the items above. Not before.

## Pre-checking the destination

Question: can the U-Boot script check `0xA0000000..image_end` against the FDT reservations before `bootm loados`
writes it? Answer from the pinned `bl33.bin` and DTBs (read on Linux, not run on a console): not in the script, yes at
build time.

| Source | What it holds | Usable by the script |
| --- | --- | --- |
| control FDT in `bl33.bin` (`fdtcontroladdr`) | no memreserve, no `/reserved-memory`, `/memory` = `0x80000000` + `0x180000000` placeholder, `/framebuffer` | no: it does not describe the free memory |
| `fdt rsvmem print` | prints the copied DTB's reservations | no: output cannot be put in a variable |
| `fdt get value` | one cell only (`reg` of 16 bytes is not returned as numbers) | no |
| `bdinfo` | real DRAM banks | no: prints only |
| `crc32 addr count dest` | the only hash in this U-Boot; the result goes to memory | not used: width and byte order of the stored word are unverified, and it is a script write |
| DTB file (`erista-sku0.dtb`) | memreserve `0x80000000..0x80020000`, `ramoops_carveout` `0xB0000000..0xB0200000`, zero size fb carveouts | yes, at build time |

Implemented: `tools/package/fdt_ranges.py` reads the reservations of the DTB the script copies and `check_destination`
refuses a loader whose span overlaps one. The script is unchanged, so the whitelist of commands is unchanged.
The `oled-sku3.dtb` has the same two reservations.

Not claimed: that the Erista's DRAM is free at `0xA0000000` beyond what the OLED showed; that the copy at `0x8D000000` is
byte equal to the pinned file (the header magic and version are checked, not a hash). A runtime hash would need `crc32`
with a destination word, which is held until its behaviour can be confirmed on the console.

## D0-pre: memory diagnostic

Two read-only entries, `iOS Diag Memory` (board, `bdinfo`) and `iOS Diag Memory FDT` (control FDT `/memory`,
`/reserved-memory`, reserve map), built by `tools/package/diag_memory.py`; see `boot/diag/memory/README.md`. They give a
person the numbers the script cannot compare: DRAM banks, `relocaddr`, `sp start`, `irq_sp`, `TLB addr`, `fdt_blob`.

`bdinfo` in the pinned `bl33.bin` (function at `0x4d64`, found from its `arch_number` label) was disassembled:
it loads `gd` and `bd` fields, calls a print helper and printf, stores only to its stack frame, and uses no system
instruction. `diag_memory_test` repeats that audit when an AArch64 `llvm-objdump` is on PATH.

D0-pre can show U-Boot's own placement and DRAM banks. It cannot show BL31's or hekate's reservations, memory-controller
carveouts or anything U-Boot does not track, so a bank with room at `0xA0000000` does not prove the range free.

### D0-pre on the Erista

- Page 2 (control FDT) ran: `/memory` is `0x80000000` + `0x180000000` (6 GiB, a fixed placeholder, now seen on the console),
  `/reserved-memory` is absent, the reserve map is empty. This matches what was read from the pinned files.
- Page 1 failed on a hush syntax error caused by an apostrophe in an unquoted `echo`. Fixed; `tools/package/hush_lint.py`
  now checks every script's characters, quotes, `${` and blocks, and `hush_lint_test` runs it over all of them. Page 1 has to
  run again before `bdinfo`'s output is known.
- Both reset paths returned to hekate on the Erista: the script's own tail (page 2) and the image's fallback after a
  failed script (page 1, `DIAGNOSTIC DID NOT RUN, NO BOOT`).

### D0-pre result: the Erista's DRAM and U-Boot's place in it

Measured (`boot/diag/erista-memory-bdinfo.log`, typed from the run's report):

| | start | size | end |
| --- | --- | --- | --- |
| DRAM bank 0 | `0x80000000` | `0x7FD00000` | `0xFFD00000` |
| DRAM bank 1 | `0x100000000` | `0x80000000` | `0x180000000` |

Bank 0 plus bank 1 is `0xFFD00000` bytes, 4093 MiB, which is 4 GiB less 3 MiB. `TLB addr 0xFFCF0000`, `relocaddr
0xFFC87000`, `reloc off 0x77B77000` (`relocaddr - reloc off = 0x88110000`, the image's link address), `irq_sp` and
`sp start 0xFF87F710`, `fdt_blob 0xFF87F728`, early malloc 138 of 2000 bytes, `boot_params 0x80000100`.

**Why 3 MiB are missing from bank 0.** `0xFFD00000..0x100000000` is the secure top of the first 4 GiB. The carveouts the
run's MC rows describe sit there with no gap, and hekate's `l4t.c` builds the same layout without NVDEC/TSEC carveouts
(`TZDRAM_BASE = 0xFFFFFFFF - 1 MiB + 1`, `GEN_CARVEOUT_TOP = TZDRAM_BASE - 1 MiB`, GPU firmware 256 KiB below it, GPU WPR
768 KiB above that):

| Range | Size | What | How known |
| --- | --- | --- | --- |
| `0xFFD00000..0xFFD40000` | 256 KiB | GPU firmware, MC carveout 2 (`0x70019c5c` = `0xffd00000`, size field 2 x 128 KiB) | decoded |
| `0xFFD40000..0xFFE00000` | 768 KiB | GPU WPR, MC carveout 3 (`0x70019cac`, size field 6) | decoded |
| `0xFFE00000..0xFFF00000` | 1 MiB | secure firmware (SC7 entry/exit, R2P, MTC table at +512 KiB, BPMP-FW), MC carveout 1 (`0x70019c0c`, size field 8) | decoded |
| `0xFFF00000..0x100000000` | 1 MiB | TZDRAM, BL31 loaded at its base, `0x700199a0` = `0xfff00000` | register meaning inferred, size from `l4t.c` |

The registers' roles are my reading of the T210 layout and `l4t.c`; the MC rows are the 25 the run reported equal to
`native_gate`'s table. The package already keeps this range out of XNU as `secure-low-tail`. That U-Boot stops its
first bank at the lowest carveout base is seen; how its code derives it is not read (the Switchroot U-Boot's source is
not in this repository). `l4t.c` prints the same value as `DRAM Bank 0 TOP`.

**U-Boot after relocation** sits in the top of bank 0: image `0xFFC87000..0xFFCF0000`, TLB `0xFFCF0000` up to the
carveouts (64 KiB assumed), FDT at `0xFF87F728`, stack growing down from `0xFF87F710`, malloc between them. Before
relocation it ran from hekate's copy at `0xAA000000` (hekate's environment below it at `0xA9FC0000`, hekate's DTB at
`0xA8000000`, per `l4t.c`).

**D0 loader `0xA0000000..0xA00151D0`** (reserved up to `0xA001EF00`; its stack is inside the image, `.stacks`):

- inside bank 0, 1.5 GiB below U-Boot's stack top, so no stack depth that U-Boot has can reach it;
- 160 MiB below the pre-relocation U-Boot, 127 MiB below hekate's DTB, 258 MiB above the boot-args page, 32 MiB below
  the staged-kernel capacity, 40 KiB below the SD reference page;
- clear of the framebuffer `0xF5A00000..0xF5E00000`, which is below U-Boot's stack top and above the loader.

`memory_layout_test` runs these comparisons over every range the package and `l4t.c` name.

hekate's T210B01 path copies its MTC table to `0xA0000000` (`BPMPFW_B01_MTC_TABLE_BASE`); the T210 path copies it to
`SECFW_BASE + 512 KiB` (`0xFFE80000`), so on the Erista nothing of hekate's is documented at the loader's address.

**Not shown.** That memory outside the listed ranges is free (a range this document does not know of); what U-Boot
holds below its stack top; the hekate DTB's size; that the loader's own execution does not touch memory outside its
span (checked by inspection of the binary, not run).


## Before the first D0 run

The memory layout is not what blocks it: the loader's span `0xA0000000..0xA00151D0` overlaps no listed range and sits about
1.5 GiB below U-Boot's stack top. What the first run is for is what no source shows: whether the EL2 to EL1 step, the
framebuffer writes and the exit work on an Erista. The risks that remain are the ones in the run table of
`boot/diag/loader/RUN_CHECKLIST.md`. Thermal exposure is the loader's few seconds plus the 15 s wait with the fan off,
about the same as the two earlier diagnostic runs, attended. The run waits for review.

## D0 result on the Erista

Run 2026-11-25, attended, hekate 6.5.5, from `More configs`. Fixture: `boot/diag/erista-d0-run.log` (transcribed from a
photograph by the reviewer). Its test is `tools/package/test/d0_run_test.py`.

| Item | Result |
| --- | --- |
| Board, EL | `T210B01=0 SKU=0 BOOT=0`, `HIDREV 00022117`, `ODM4 00000204`, `EL1 STATE PASS`, `CNTFRQ=19200000`, `HCR=80000000` |
| Entry SP | `FF87D010`: `0x2700` below U-Boot's `sp start` `FF87F710` from bdinfo, so inside U-Boot's own stack |
| MC | 25 rows, in `native_gate`'s order, equal to its table, all SAME |
| Exit | `RESET IN 15 S`, then hekate. PSCI `SYSTEM_RESET` from EL1 and the `wfe` wake are seen to work once |

MC at U-Boot time against loader time. The first diagnostic reported 25 same, 0 different against the same table, and
`erista-first-run.log` holds the table's values, not typed rows. So equality of the two moments follows from both runs
matching one table, not from two transcripts. Run `diag_boot.py adopt` with the first run's `LOG.txt` to make it a
comparison of two measurements.

The carveout rows decode to the layout bdinfo showed: secure firmware `FFE00000` and 8 units of 128 KiB (to `FFF00000`),
GPU firmware `FFD00000` and 2 units (to `FFD40000`), GPU WPR `FFD40000` and 6 units (to `FFE00000`), TZDRAM base
`FFF00000`. They end where bank 0 ends plus 3 MiB, as `memory_layout_test` already checks.

GIC words, read as a GIC-400. Checked against Linux (`include/linux/irqchip/arm-gic.h`, `drivers/irqchip/irq-gic.c`, master): the offsets `0x00`, `0x04`, `0x100` and `0xFC`; the `GICC_CTLR` bits (enable group 0 is bit 0, the four bypass disable bits are `0x1E0`, so `0x61` is bit 0, 5 and 6); the interrupt count (`(TYPER & 0x1F) + 1` times 32); and the GICv2 test `(IIDR & 0xFF0FFF) == 0x02043B`, which `0x0202143B` passes. The field widths of `GICD_TYPER` above bit 4 (CPU count, security extensions, lockable SPIs) and the product ID in `GICC_IIDR` bits 31 to 20 are from the GICv2 specification as remembered, and no copy of the specification was read for them. `native_gate` records these
words and expects none, and the repository has no OLED values for them, so there is nothing to compare with except that the
six addresses equal the normal loader's.

| Word | Value | Reads as |
| --- | --- | --- |
| `GICD_CTLR` | `00000001` | group 0 forwarding on |
| `GICD_TYPER` | `0000FC66` | 224 interrupt lines, 4 CPU interfaces, security extensions, 31 lockable SPIs |
| `GICD_ISENABLER0` | `0000FEFF` | SGIs and PPIs 0 to 7 and 9 to 15 enabled, 8 off, 16 to 31 off |
| `GICC_CTLR` | `00000061` | group 0 enabled, IRQ and FIQ bypass disabled |
| `GICC_PMR` | `00000080` | priority mask `0x80` |
| `GICC_IIDR` | `0202143B` | Arm, GIC architecture version 2, product `0x020` (GIC-400) |

FDT. Derived from the source versus measured:

| Quantity | From the source | Measured |
| --- | --- | --- |
| Banks 2 | the walk requires `/memory` to equal `0x80000000+0x7FD00000` and `0x100000000+0x80000000` (`memory.c`) | equal to U-Boot's own banks (bdinfo): the packaged DTB and U-Boot agree |
| Nodes 3 | `/reserved-memory` children `ramoops_carveout` (`0xB0000000..0xB0200000`), `fb0_carveout`, `fb1_carveout` (reg all zero in the DTB bytes, so no range) | the count only |
| Reservations 3 | reserve map entries: one in the DTB (`0x80000000..0x80020000`), one the script adds (framebuffer `0xF5A00000..0xF5E00000`) | the count only; one entry has no source here |
| Total 172032 | the DTB is 168190 bytes; `0x2A000` is page aligned | the number only |

The third reserve-map entry is the blob itself (`0x8D000000`, `0x2A000`), shown by D0-R and explained in
`ERISTA_D1_DESIGN.md` from the pinned binary's machine code; the walk reports no overlap with the loader's five ranges.

What this does not show: thermal protection (the fan was off and no reading was taken), memory that no list names, the
hekate DTB's real size.

## D0-R result on the Erista

`boot/diag/erista-d0r-run.log`, read by `d0r_run_test`. Banks, `MAP 0` to `MAP 2`, `NODE 0` and the two areas are in
`ERISTA_D1_DESIGN.md`. `CLEAR? BANK 0` is `CLEAR, BANK 0` drawn with a font that has no comma. The entry stack was
`FF87DD10`, not D0's `FF87D010`; both lie below U-Boot's stack start. The six GIC words equal D0's. No overlap with any
reservation on the screen or any range this repository can name is not proof that memory is free.
