# D1 design and the temperature plan

For review. D1 itself is not implemented (D0-R and D1-pre are, and have run). `boot` stays 0 for the Erista; D2, XNU, voltage changes and an EMC raise are not
part of it.

## What D0 settled and what it did not

Settled by the run (`boot/diag/erista-d0-run.log`): the loader reaches EL1 on the Erista, reads 25 MC rows equal to the
table, reads the GIC, walks the DTB without finding an overlap and resets to hekate through PSCI from EL1.

Not settled, and D1 inherits it: thermal protection, memory no list names, the hekate DTB's size, whether the exporter's
view of U-Boot's objects holds on this console. D1-pre and D0-R have since answered the card's visible state and the
reserve map (below).

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
  blocks. `mmc info` showed SD 3.0, high capacity, 512 byte blocks and a 4 bit bus; single data rate, OCR, CSD version and
  the rest are not on its screen.
- The memory around `0xA7000000` (64 KiB of exporter and its 16 KiB stack) and `0xA001F000..0xA0020000` beyond the
  loader's range. D0-R found no overlap with any reservation it can list, which is not proof the memory is free.

## Exporter audit (pinned `exporter.uimg`, `7c33bbdc...`)

From the source and from its machine code (`tools/package/exporter_audit.py`, run by `exporter_audit_test`):

- RAM it writes: the image and its own stack, `0xA7000000..0xA7005E50` (24144 bytes; the 16 KiB stack is a `.private` array
  inside the image and `saved_caller` sits beside it), and the record, `0xA001F400..0xA001F500`: first the magic word is
  zeroed, bytes 4 to 255 follow, then the magic. The linker script asserts at most 64 KiB and no `.bss`.
- Stack: its own, set by `_start` from `stack_top`; the caller's SP, LR and `x18` are saved and restored.
- System instructions: three `dsb sy` and nothing else (no `msr`, `mrs`, `dc`, `ic`, `tlbi`, `svc`, `hvc`, `smc`, `eret`,
  `wfe`). Register branches: 14 `blr`, all calls through the reader's callback pointer, which is loaded from a structure in
  the image; no `br`.
- Device addresses: none is formed except `0x700B0000`, which is stored in the record and compared against, never
  dereferenced. The one other constant in that range is part of the CRC32 routine.
- Indirect access: every read of U-Boot's objects goes through `copy_ram`, after `span()` has required 8 byte alignment and a
  range inside `0xF0000000..0xFFD00000`, which is DRAM. A pointer taken from a U-Boot object that points at a peripheral is
  refused. The measured Erista layout fits those bounds: `relocaddr 0xFFC87000`, the image's `0x68000` bytes end at
  `0xFFCEF000`, below the TLB at `0xFFCF0000`; the heap `0xFF885000..0xFFC87000` sits just above `sp start`; and
  `relocaddr - reloc off = 0x88110000` is the link address the exporter's offsets assume.
- The record carries the card's CSD and CID (32 bytes at `0x40`), the RCA, and four U-Boot addresses (`mmc`, descriptor,
  `gd`, relocation base). The CID holds the serial number. A record or a photograph of one is never published; a fixture
  keeps the fields the checks need and masks the rest (`sd_requirements.mask`).

What the audit does not show: that the object offsets hold on this fork of U-Boot on an Erista. The 277 instruction
signatures against the pinned binary prove the code matches. Whether a live card's objects are where they say is only seen by
running it.

## Steps, each reviewed before the next

**D1-pre (run once, both pages): display only, U-Boot only, no loader.** Two hekate entries, `boot/diag/sd/README.md`:
`iOS Diag SD` (`devnum`, `mmc list`, `part list mmc 1`, the way the exporter step's `read mmc 1` reaches the card) and
`iOS Diag SD Info` (`mmc dev 1`, `mmc info`; `mmc dev` starts the card again, so it is its own entry). `sd_requirements.py`
judges the `mmc info` text against the exporter's conditions and says which it cannot show. The command list is an allowlist
with a test.

**D1: the D0 image plus the exporter and the SD reference.** Changes against D0:

- The script template takes the exporter, SD reference and record steps from `boot/u-boot-script.txt` (the same lines, so
  the Erista runs what the OLED runs). The status names stay distinct from D0's.
- The loader prints the record's fields (CID, CSD, RCA, capacity, bus width) and the result of `sd_reference.c`'s
  validation. D0-R already prints every reserve-map entry's base and size; D1 keeps those lines.
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

Hardware: one attended, filmed run with the stage deadlines of the D1 checklist (written with the D1 build). D1-pre has
shown the card's visible state meets the exporter's conditions, except the ones `mmc info` cannot show. Success is the
loader's printed record equal to what D1-pre's `mmc info` showed. Any refusal is reported and stops the work.

## Temperature

Findings and sources: `ERISTA_THERMAL.md`. What changes the plan:

- The pinned U-Boot disables I2C1 and the PWM block in its control FDT. It has no bus to the TMP451 and no fan device, so
  the earlier T1 ("`i2c md` on the TMP451, read only") is not available. Any TMP451 read needs an I2C controller
  transaction, so it is a peripheral write step, and the value after Nyx's `hw_deinit` is stale (shutdown mode).
- hekate's fan, 5 V and TMP451 code has separate Erista branches (tachometer pull-up, remote offset 0 degrees C, GPIO CC4);
  this repository's driver is the Mariko path.
- Whether the hardware thermtrip is armed at handoff is not known; it is treated as absent.

Plan, each step its own review: T1 (communication check by one TMP451 read, via a patched FDT or a loader driven I2C1
transaction; stale value, never a temperature or a safety verdict), T2 (clear shutdown or one shot conversion), T3 (fan,
from hekate's Erista branch), T4 (a bound for longer runs: SOC_THERM trip or a watchdog). None is implemented. D0-R and
D1-pre run without any of them and are bounded by what they execute (stage deadlines in their checklists).

## Measured on the Erista (2026-11-25)

Fixtures: `boot/diag/erista-sd-pages.log` (D1-pre pages 1 and 2, maker, OEM and name masked) and
`boot/diag/erista-d0r-run.log`. The reviewer transcribed both from photographs, which were not seen here. Tests:
`diag_sd_test` (class `Erista`), `d0r_run_test`, `bl33_reserve_audit_test`. All three screens ended with hekate returning by
itself.

| Question | Answer | Status |
| --- | --- | --- |
| Is the card in a mode the exporter accepts? | Page 2: `SD version 3.0`, `High Capacity: Yes`, `Rd Block Len: 512`, `Bus Width: 4-bit` (no `DDR`), 29.8 GiB, under 2^32 sectors. | Five conditions MET. Single data rate NOT SHOWN: `mmc info` prints no bus mode, and `Tran Speed: 50000000` agrees with single rate without reporting it. Five more NOT SHOWN (OCR with CCS, CSD version 2, RCA and CID nonzero, capacity equality, controller and driver structure). |
| Is `devnum` 1 and is the card block device 1? | Page 1: `devnum=1 boot_targets=mmc1`, `sdhci@700b0000: 1 (SD)`, `sdhci@700b0600: 0`, one DOS partition from sector 8192, 62406656 sectors, type `0c`. | Measured. The partition ends inside the 29.8 GiB the card reports. |
| Do the exporter area and the SD reference page clear every reservation? | D0-R: `EXPORTER 00A7000000 00A7010000` and `SD REFERENCE 00A001F000 00A0020000`, both clear, both in bank 0. | No overlap with the DTB's memreserve, the script's framebuffer entry, the blob's own entry or the ramoops node. Not proof of free memory (below). |
| What is the third reserve map entry? | `MAP 2 008D000000 000002A000`: the blob itself, at its total size (172032, `FDT 8D000000 TOTAL 172032` on the same screen). | Origin read from machine code, not from source (below). |

The two banks are U-Boot's own (`0x80000000+0x7FD00000`, `0x100000000+0x80000000`). `MAP 0` is the DTB's memreserve
(`0x80000000..0x80020000`), `MAP 1` the script's `fdt rsvmem add 0xF5A00000 0x400000`, `NODE 0` the ramoops carveout
(`0xB0000000..0xB0200000`). The entry stack differed from D0's (`FF87DD10` against `FF87D010`) and stayed below U-Boot's
stack start; the loader does not depend on its value.

The loader's `CLEAR? BANK 0` is `CLEAR, BANK 0`: the display font has no comma and draws `?`. Only that string has a comma.
D0-R is a tested artifact and stays as it is; D1's loader drops the comma.

### How MAP 2 gets added

Upstream U-Boot (master `9d67454`) does not explain it. `fdt_shrink_to_minimum` there deletes the blob's own entry and
adds it back only if it found one, and the packaged DTB has none. The pinned `bl33.bin` is `U-Boot 2024.NX02.b201801`, a
fork whose source is not in this repository, so its machine code is the evidence (`tools/package/bl33_reserve_audit.py`,
run by `bl33_reserve_audit_test`):

- `fdt_add_mem_rsv` has three callers: `fdt rsvmem add`, the initrd fixup and one function that matches
  `fdt_shrink_to_minimum` (it deletes entries whose address is the blob, holds the constant `0x104F` for five spare entries
  and a page, writes the new `totalsize`).
- In that function no conditional branch stands between the loop's exit and the call to `fdt_add_mem_rsv`, so the blob's own
  entry is added whether or not one was deleted. That differs from upstream master and gives `0x8D000000` plus the page
  aligned total, `0x2A000`.
- The function has two callers. One passes extra size 0 straight after a call that precedes it in a function carrying the
  `fdt_high` handling, which is where `image_setup_libfdt` calls it in upstream during `bootm prep`. That matching is by
  structure, not by a symbol.

This was read, not run. The `fdt resize` command is the other caller. Neither the fork's source nor an execution trace
confirms the path; the screen only agrees with it.

### What the no-overlap result covers

`d0r_run_test` compares the two areas with every range this repository can name (`memory_layout.regions`) and with the
screen's own `MAP` and `NODE` lines. The SD reference page touches none. The exporter's 64 KiB lies inside
`staged-kernel-capacity` (`0xA2000000..0xA8000000`), the package's own exclusion, which `loader_abi.EXPORTER_RUN` documents
as the exporter's home because the kernel window rides in `bl33.bin`. D1 stages no kernel, so nothing else claims it.
That is a list of known occupants. Memory that no list names stays unverified.

## Final review before D1 is written

D1 is the D0-R image plus the normal script's exporter, `read mmc 1` and `go` steps, and the loader reading the record and
running `sd_reference.c`'s validation. No write to the card, no loader access to `0x700B0000`. What is settled above
removes three of the four open questions; the exporter and the unlisted memory remain. D1's code waits for approval of the
requirements below.

### Before D1 runs: the payload

Design for review. Constants come from the pinned files through the script template, never from literals in the script.

**Integrity.** The U-Boot script uses `verify no`, so U-Boot checks nothing it loads. CRC32 is the only hash U-Boot here has
(`crc32`; no `sha256sum`). It detects corruption, not tampering, which the build's hashes and the card's own protection
cover.

| Where | Check | On failure |
| --- | --- | --- |
| Build | `exporter.uimg` SHA-256 equals its pin in `bazel/pinned/SHA256SUMS` (`7c33bbdc...`); header magic, OS 17, architecture 22, type standalone, no compression, load and entry `0xA7000000`, name `Switch MMC RAM Export v1`; the header's data CRC (`0x630D1CB8`) equals the CRC32 of the payload; payload `0x5E50` bytes, at most the 64 KiB of `EXPORTER_RUN` and the exporter slot; `exporter_audit` passes on those bytes | the build fails |
| Build | `bl33.bin` SHA-256 equals its pin (`b492a401...`) | the build fails |
| Build | the cache helpers' words are read from that `bl33.bin` at `0x1920` (`d508711f d5033fdf d65f03c0`, `ic ialluis; isb; ret`) and `0x1A64` (`d5033f9f d5033fdf d65f03c0`, `dsb sy; isb; ret`; not a function start, the tail of the TLB invalidate, see `ERISTA_CACHE.md`) | the build fails |
| Run, after `bootm loados` | `crc32 0xA7000000 0x5E50 <scratch>`, then `itest.l *<scratch> == 0x630D1CB8` | `EXPORTER_IMAGE_CORRUPT`, no `go` |
| Run, before each helper `go` | `itest.l` the three words at `0xAA001920`, then at `0xAA001A64` | `CACHE_HELPER_MISMATCH`, no `go` |
| Run, immediately before `go 0xA7000000` | the same `crc32` and `itest.l` again; the loader and `bootm prep` run between load and `go`, so the first check is not enough | `EXPORTER_IMAGE_CORRUPT`, no `go` |

The scratch word sits at `0xA001F600`, inside the SD reference page D0-R found clear and the script clears next.

**Load size.** The header says `0x5E50` (24144) bytes. The run area is 64 KiB (`0xA7000000..0xA7010000`), which holds the
payload and the 16 KiB stack inside it; D0-R's `EXPORTER` line covers the whole 64 KiB. The build requires payload plus
header at most the exporter slot (`0xAA0B0000`, 64 KiB) and payload at most the run area.

**Placement.** Exporter `0xA7000000..0xA7010000`. SD reference page `0xA001F000..0xA0020000`, of which the script clears
`0x1000` bytes, reads sector 0 to `0xA001F200..0xA001F400` and expects the exporter's record at
`0xA001F400..0xA001F500`. D0-R found both areas clear of every reservation on its screen. `check_destination` gains both
as ranges the build refuses to overlap with the loader, DTB slot and exporter slot.

**SD read failure.** One attempt, no retry, nothing written to the card. In order, each a distinct status and a `REFUSED`
line that waits the dwell and resets (stage deadline 25 s from the line):

| Step | Failure status | Left in RAM |
| --- | --- | --- |
| `${devnum}` is not `1` | `SD_REFERENCE_WRONG_DEVICE` | nothing new |
| `mw.b 0xA001F000 0 0x1000` | `SD_REFERENCE_CLEAR_FAILED` | the page may be partly cleared |
| `read mmc 1 0xA001F200 0 1` | `SD_REFERENCE_READ_FAILED` | up to 512 bytes at `0xA001F200`; `go` is not run |
| `go 0xA7000000` returns nonzero | `SD_CARD_EXPORT_FAILED` | the exporter ran and may have written its record at `0xA001F400` |
| magic `0x4d435349`, version 1 or size `0x100` mismatch | `SD_CARD_RECORD_FAILED` | the record bytes as the exporter left them |

The first read of the card's state by U-Boot's driver is `read mmc 1`, so a card that fails there fails before the exporter
sees it. The exporter's own refusals (mode, OCR, CSD, RCA, CID, capacity, controller) arrive as the nonzero `go`.

**Stage deadlines.** The SD read: 15 s. `go 0xA7000000`: 10 s. Past either, hold the power button, as in the D0 checklist.
The exporter is the first code outside the diagnostic's own image that D1 runs, so a hang is possible, not only a refusal.

### Other D1 requirements, unchanged

- The loader prints field names and decoded values from the record; the CID, CSD and the four U-Boot addresses are never
  printed in full. A fixture keeps masked values.
- The link inspection is D0's plus a rule that no load or store reaches `0x700B0000`, from the disassembly.
- A test that the D1 script's commands are the normal script's exporter lines plus the checks above and nothing else (no
  `saveenv`, no `mmc write`, no `mw` outside the two documented ranges).
- `RUN_CHECKLIST.md` rewritten for D1 with the build's hashes.

### Still unverified, and not made less so by anything above

- The exporter on this console. D1 is the first run to execute code from outside the diagnostic's own image (`go` into a
  loaded payload), so the first failure may be a hang.
- Thermal protection: the fan is off, no sensor is read, the hardware trip is not known to be armed.
- Memory that no list names, `0xA7000000` and `0xA001F000` above all. D0-R's no-overlap is against known ranges only.
- Single data rate and the five other conditions `mmc info` cannot show.
- The fork's U-Boot source, and therefore the exact path that adds `MAP 2`.

## Questions for the reviewer

1. Are the integrity checks above enough for `verify no`, or should D1 also run `bootm` with `verify yes` for the exporter?
2. Is a one-shot read of 512 bytes acceptable as the only SD access, given that a failing `read mmc 1` has already
   initialized the controller?
3. T1's stale reading: accepted as useful, or should T1 wait for T2?
4. Should the `mc_verified` change travel with D1 or alone? (The reserve-map print already shipped with D0-R.)

## D1 as implemented (not run on hardware)

Code in `boot/diag/sd_loader/`, packaged by `//boot:diag_loader_sd` as the entry `iOS Diag Loader SD`. `README.md` there
says what the script does; `RUN_CHECKLIST.md` is the run. Where it differs from the table above:

- The measured CRC32 values and the step flags go to a status block at `0xA700F000` (256 bytes, inside the exporter's 64 KiB
  run area, above the 24144 byte image and clear of the exporter's writes), not to a scratch word at `0xA001F600`. The
  loader reads the block; its magic is written last.
- The exporter is checked in four places: the build (pin, header, data CRC, size), its slot before `bootm start` (all 16
  header words, then the data CRC32), `bootm start` with `verify yes`, and at `0xA7000000` twice (after `bootm loados`,
  and again right before `go`).
- The helpers are called with `go`, after the exporter is loaded and before the loader is. Each returns 1 and leaves `x0`
  alone, so the script requires `go` to report failure.

### Answers to the reviewer's questions

1. `verify yes` works in the pinned `bootm start`: the header CRC is always checked, the data CRC only when `verify` is
   nonzero, and it covers the copy in the slot, not the copy at `0xA7000000`. D1 sets it for the exporter only and returns to
   `verify no` before the loader's `bootm`. The CRC32 checks at the run address stay. CRC32 is not tamper protection.
2. One read is the only data access: sector 0 of `mmc 1`, 512 bytes, no retry, no `mmc0`. The card-initialization commands
   U-Boot's driver issues inside that read are not separately observable and are recorded as such in the checklist.
3. T1 is not part of D1. A stale TMP451 value is not used for any safety decision. T1 to T4 continue as separate work.
4. `mc_verified` is separate and unchanged.

### What the tests show, and what they cannot

| Test | Shows |
| --- | --- |
| `diag_sd_loader_test` | The real script on `script_sim.py`: a clean run hands off with 10 of 10 steps; one injected failure per status (wrong board, status block, devnum, header size, slot data CRC, helper words, `bootm start`, `bootm loados`, image corrupt after load, helper `go` reporting success, page clear, read, image changed after the read, exporter return, bad record, metadata) ends in `reset`, with its own status, no `bootm go`, the exporter's `go` only when every gate before it held, and at most one `read`. Also the command allowlist, the single `read mmc 1`, the `go` targets, the `mw` ranges, the package contents and the inspection's SD rules. |
| `sd_report_test` (C++) | The status block and reference page evaluation: a valid run, a refused page, a missing block, fewer than 10 flags, a changed CRC, no identity in any line, line width. |
| `exporter_audit_test`, `test_export.py` | The exporter's machine code and its pins, unchanged. |
| QEMU `model.py` (Mac only, needs `qemu-system-aarch64`) | The exporter's machine code on synthetic firmware RAM. It does not run U-Boot or the script, so it cannot show the `go`, the caches or the card. |

`script_sim.py` models the commands the script uses. It is not U-Boot: it shows what the script does when a command
fails or RAM differs, not what the hardware does.

Still unverified: the exporter on this console (the first `go` into a loaded payload may hang); the I-side freshness of
the exporter's bytes (`ERISTA_CACHE.md`: `bootm loados` cleans the D-cache with `dc civac`, `go` does nothing, and the one
`ic ialluis` that makes the fetch side current is the first helper; no U-Boot command shows the cache state); thermal
protection (fan off, no sensor read, T1 excluded, no trip known to be armed; `ERISTA_THERMAL.md`); memory no list names;
single data rate and the other conditions `mmc info` cannot show.

### What the indirect MMIO audit guarantees

`diag_loader_inspect.py` reads the link map and the disassembly. It guarantees, for the code in the image:

- no instruction forms an address in `0x50000000..0x7fffffff` (as `movz`/`movk` pairs or a whole `mov` immediate), except
  the identity registers' `0x7000` page and, in the validator only, `0x700B0000` as a compare constant;
- the controller constant is never loaded through, stored through or copied (tracked per register, in the three validator
  functions);
- the only device read is the single load in `switch_diag_read32`, and its only call sites are in
  `switch_diag_after_transition`, with addresses from `mc_table.h`, the GIC list and the two identity registers;
- no `ic`, `dc`, `hvc`, `svc`, `sys`, `at`, `wfi`, `sev`; the `msr` targets and counts are those listed;
- a source scan (`diag_loader_test.py`) rejects a volatile cast or a device address outside that function and the report
  code. A scan of the linked D1 image finds device range words in `.rodata` only as the memory controller table
  (`0x70019xxx`) and the six GIC addresses (`0x5004xxxx`), none in `.data`, and the one literal-pool load in code is
  `SCTLR_EL1`'s value.

What it cannot guarantee: an address that comes from data. The loader reads the DTB pointer, the FDT, the exporter's record
and the reference page, and a value read from one of them could in principle be used as a pointer. The code does not do it
(the validator compares the record's controller field and nothing else), but a binary scan does not prove it; the review of
`sd_report.c` and `sd_reference.c` does. It also cannot see an address built by arithmetic from a register other than the
patterns above, a literal pool of a different shape, or what U-Boot and the exporter do. For the exporter the equivalent
statement is `exporter_audit_test`'s: no device address formed, every read of firmware objects range-checked to DRAM.

### What the QEMU model can and cannot show

| Can | Cannot |
| --- | --- |
| The exporter's exact machine code on a generic Cortex-A57 with synthetic firmware RAM: the record's bytes and fields, the refusals (bad `gd`, heap mismatch, class and device faults, card links, state, capacity), that firmware RAM is unchanged, that `x18` and the stack are preserved | U-Boot, the script, `bootm`, `go` as U-Boot runs it, the helpers |
| That the image returns, and what it returns | Caches (TCG models none), so any stale I-side line |
| That the 277 instruction signatures match the pinned `bl33.bin` | A real card, the SDHCI controller, DMA, timing |
| | The loader, the status block, the handoff, the screen |
| | Thermal behaviour, a hang on this console |

`model.py` and `test_export.py` run on the Mac only (they need `qemu-system-aarch64`). `script_sim.py` covers the script's
control flow for the commands it uses, and nothing about the commands themselves.
