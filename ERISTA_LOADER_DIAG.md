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

Goal: run the real loader path up to `native_gate`'s moment on the Erista, print what it sees, and stop. It does not lift
`boot`, carries no kernel and cannot start XNU.

Shape:

- A second loader build, `LOAD_DIAG`, from the same sources. It has no kernel window, no copy, no `loader_enter_kernel`: those
  files are not linked, so there is nothing to enter. `loader_after_transition` is shared up to the point where `boot` is
  checked, and the diagnostic returns the board row instead of refusing on `boot 0`.
- Its own hekate entry and `bl33.bin` (the same pattern as `diag_boot.py`): the pinned U-Boot, a script that does what the
  normal one does up to `bootm prep` and `bootm go` into the diagnostic loader, and nothing after. No `bootargs.txt`, no
  exporter in the first stage.
- It prints, on the framebuffer the loader already draws on: the board row and its name; HIDREV and ODM4; the EL state words
  of 0x301; entry SP against its range; the 25 registers with `same` or `DIFFERENT` against the OLED table (the same lines
  as `diag_boot.py compare`); the six GIC words; ISAR0; the FDT memory report (banks, reservations, the range checks of
  0x500 and 0x740 with their results); then the 15 second wait and `smc #0` with PSCI `SYSTEM_RESET`, the call U-Boot made when
  the first run landed in hekate. No refusal exits early: each check is printed and the run goes on.
- Hardware writes: framebuffer RAM, the loader's own RAM, CPU system registers for the EL change and caches. None to a
  peripheral. The list goes into the review, as the diagnostic's did.

Stages, each reviewed before the next:

1. `D0`: everything above, no SD, no exporter.
2. `D1`: the exporter and the SD reference validate, read only (the Erista's card).
3. `D2`: the staged window's header, trailer and CRC, with the copy left out.

Tests before any console run: host tests for the report text and the compare (the existing `memory_*` and `emc` model tests
show the pattern), a test that the diagnostic binary has no `loader_enter_kernel` symbol and no store to the MC block, and the
Python checks on its image the way `diag_boot_test.py` does them.

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
| Temperature | nothing reads or drives the fan or the TMP451. The kernel's watchdog service touches no hardware (`ERISTA_RESEARCH.md`, section 5), so a hung kernel is not bounded by anything | the TMP451's U-Boot bus number; a fan and temperature plan before a kernel runs for more than seconds |

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
