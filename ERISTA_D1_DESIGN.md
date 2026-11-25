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

**D1-pre (implemented, not run): display only, U-Boot only, no loader.** Two hekate entries, `boot/diag/sd/README.md`:
`iOS Diag SD` (`devnum`, `mmc list`, `part list mmc 1`, the way the exporter step's `read mmc 1` reaches the card) and
`iOS Diag SD Info` (`mmc dev 1`, `mmc info`; `mmc dev` starts the card again, so it is its own entry). `sd_requirements.py`
judges the `mmc info` text against the exporter's conditions and says which it cannot show. The command list is an allowlist
with a test.

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

Hardware: one attended, filmed run with the stage deadlines of the D1 checklist (written with the D1 build), after D1-pre has shown a card in the mode
the exporter accepts. Success is the loader's printed record equal to what D1-pre's `mmc info` showed. Any refusal is
reported and stops the work.

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

## State after D1-pre and D0-R (2026-11-25)

All three ran and hekate came back by itself each time. The contents were not supplied, so what they were meant to show is
still open:

| Question | Answer that the screens hold | Known now |
| --- | --- | --- |
| Is the card in a mode the exporter accepts (SD, high capacity, 512 byte blocks, 4 bit single rate, size)? | page 2's `mmc info` | not read |
| Is `devnum` 1 in this script context, and is `part list mmc 1` readable? | page 1 | not read |
| Do the exporter area and the SD reference page clear every reservation, inside a bank? | D0-R's last two lines | not read |
| What is the third reserve map entry? | D0-R's `MAP` lines | not read |

## Final review before D1 is written

Do not write D1 yet. Three things decide its shape and none is in hand:

1. The `mmc info` and `part list` text. If the mode fails a condition, the exporter will refuse, and D1 becomes a different
   problem (a card or a mode change) rather than a run.
2. D0-R's lines. An overlap or the third entry's origin could move the exporter's or the record's address.
3. A decision on the unverified exporter step, which cannot be tested short of running it.

What the design holds regardless, and is ready to implement once 1 and 2 are read:

- D1 is the D0-R image plus the normal script's exporter, `read mmc 1` and `go` steps, copied line for line, and the loader
  reading the record and running `sd_reference.c`'s validation. No write to the card, no loader access to `0x700B0000`.
- Build time: `check_destination` covers both areas against the DTB (it covers the exporter area now), the link inspection
  is D0's plus a rule that no load or store reaches `0x700B0000`, a test that the D1 script's commands are the normal
  script's exporter lines and nothing else.
- Record handling: the loader prints field names and decoded values; the CID, CSD and the four U-Boot addresses are never
  printed in full. A fixture keeps masked values.
- Deadlines: the SD read and the exporter are the new stages, 15 s for the read and 10 s for `go`, hard cut by the power
  button, and no retry on the spot.

Still unverified, and not made less so by anything above: thermal protection (the fan is off, no sensor is read, the
hardware trip is not known to be armed); memory no list names, around `0xA7000000` and `0xA001F000` above all; the exporter on
this console. D1 is the first run that executes code from outside the diagnostic's own image on the Erista (`go` into a loaded
payload), so the first failure there may be a hang, not a refusal.

## Questions for the reviewer

1. Is D1-pre worth its own run, or should its commands be the first lines of D1's U-Boot half? Separate is cheaper to
   abort; combined is one fewer card read.
2. T1's stale reading: accepted as useful, or should T1 wait for T2?
3. Should the reserve-map print and the `mc_verified` change travel with D1 or alone?
