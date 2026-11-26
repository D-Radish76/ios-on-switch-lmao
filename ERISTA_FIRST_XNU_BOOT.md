# Erista: the first XNU boot, as an observation package

Design (N7-F). Nothing here is approved to run. `boot` stays 0 and the feature mask 0, so both stages refuse an Erista console,
and every hardware HOLD stays. iPadOS 17 (xnu-10063.141.1) only.

The aim is to see how far XNU gets, with the console's own screen and nothing else (no UART). It is a package of its own, not a mode of
the normal one.

## What is in it

| Part | Setting | Why |
| --- | --- | --- |
| Target | `//boot:erista_first_xnu` (manual), directory `erista-first-xnu` | the same attributes as `//boot:erista_package` under another name, so a trace build never overwrites the plain one (`erista_package_test`) |
| Kernel | debug profile (the default), `--//kernel:early_trace=true` | start.s draws every early mark and `SwitchDiag.c` the C stages'; release kernels never do (`early_trace_test`) |
| Cores | one (`//boot:single_core`, Erista ADT with a single CPU) | PSCI `CPU_ON` has not been run on this board |
| Screen | the framebuffer the loader already checks: `0xF5A00000`, 720x1280, stride 2880 | no display controller access, no new mapping beyond start.s's own |
| Features | mask 0: no PMIC, no MC error interrupt, no EMC request, no sensors, no SE | the loader's EMC raise, the PMIC and MC writes and the thermal service are all behind features (`ERISTA_BOOT_AUDIT.md`) |
| Peripherals | the Erista ADT's audited nodes only | `ERISTA_ADT.md` |
| Added hardware writes | none by this package. The debug kernel adds WDT4 only when `watchdogd` connects, which needs userspace | audited in `early_trace_test` |

Mac build, unverified (the flag and the target have not been built):

```
bazel build --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true //boot:erista_first_xnu
```

Under the release's prebuilt parts the bl33 stage refuses the target (constants differ), so `prebuilt=local` is required.

The target sets `require_early_trace`. Analysis fails under `--config=release`, and an action reads the built kernel
(`tools/package/first_xnu_check.py kernel`) for start.s's trace enable sequence, so a build without
`--//kernel:early_trace=true` stops there and writes no package. The scan is for four instruction words in the kernel's
text. It is host-tested on synthetic bytes only. On the Mac, once, to see it work on real kernels:

```
bazel build --config=ios17 --//kernel:early_trace=true //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel bazel-bin/kernel/<the prelinked kernel>   # exit 0
bazel build --config=ios17 //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel bazel-bin/kernel/<the prelinked kernel>   # exit 1
```

The loader's room below U-Boot's stamps slot (`0xa001ef00`) and the SD reference page, from the built image (the link
already fails past either):

```
python3 tools/package/first_xnu_check.py margin bazel-bin/boot/erista_first_xnu_work/loader.uboot.bin
```

It reports the flat image only. Stacks are not in the file, so `loader_link.py`'s footprint check stays the authority on those.

## What the screen shows

Each mark is a 16x16 green block at y 280, x `16 + 40 * (stage - 1)`. Stages 1 to 6 are drawn by `start.s`, 7 to 21 by
`switch_diag_early_stage`. A fault replaces the sequence with the early exception screen, whose LAST STAGE line names the stage.

| Checkpoint | Mark or line |
| --- | --- |
| XNU entry | 1 (video contract held, early rendering on), 2 (boot args read) |
| MMU init | 3 (framebuffer block mapped), 4 (before the MMU is enabled), 5 (MMU on, KVA vectors live) |
| Exception vectors | physical vectors before 4, KVA alias at 5; the early exception screen is armed from mark 1 |
| Before the branch to `arm_init` | 6 |
| `arm_init` | 7 entered, 8 image rebased |
| `PE_init_platform` | 9 |
| CPU and clock bring-up | 10 to 15 |
| Memory init | 16 `arm_vm_init` entered, 17 bounds, 18 random slide (the first use of the random seed), 19 and 20 final tables, 21 last |
| VM initialised | `XNU VM INITIALIZED` (below). The old HUD does not run in this configuration; the marks stay |
| IOKit init | not reached: the diagnostic resets the console first (`switch_stop_after_vm`) |
| First storage access | not reached for the same reason. Without the stop it would be the SD driver (`switch_root_event`), unchecked on screen |
| GIC init | no mark. The nearest line is `platform interrupts initialized` after `PE_init_platform(TRUE)`. Whether that covers the GIC distributor setup is not verified |

The marks stay on screen: `SwitchVmDone.c` clears only a band at columns 20 to 150.

If no mark appears at all, one of these holds, and the screen cannot tell them apart: the loader or XNU hung before mark 1, or start.s's
contract (`BA_PHYS_BASE`, 4 GiB memory size, virtual base, `0xF5A00000`, stride 2880) did not match the Erista's boot args and early
rendering stayed off. The loader's stamps at `0xa001ef00` (`ERISTA_XNU_HANDOFF.md`) are the second source. The boot args against that contract are compared in the next section.

## Boot args against start.s (N7 follow-up A)

`boot_args_contract_test` evaluates start.s's early-rendering gate on the values the packager writes. Agreeing: field
offsets (`0x28` to `0x50`), `physBase` `0x80000000`, `memSize` 4 GiB, the kernel base `0xb207c000`, `virtBase` derived from
the kind's `kernel_virt_base`, framebuffer `0xF5A00000` with stride 2880 inside measured DRAM bank 0
(`0x80000000` to `0xFFD00000`) and in the loader's reservations, and every mark (stages 1 to 21, x up to 831 in the
logical 1280x720 frame) inside the 0x384000-byte framebuffer. A mark clobbers only x16 to x18, uses no stack and no call,
and draws physical `0xF5A00000` addresses: identity-mapped as Device by the stage 3 block before the MMU is enabled, and
through that same TTBR0 block after.

**The mismatch, closed for the trace build (N7-G).** The packager's Video is display 1 with depth `0x320`. XNU decodes that word as depth 32 (bits 0 to 7),
rotate 3 (bits 8 to 15), scale 1 and no extra boot rotation (`pe_init.c`, `kBootVideoDepth*Shift`), which is the landscape orientation the marks
are drawn in. start.s accepted display 1 with the bare 32 only, so the gate stayed shut. Under `SWITCH_EARLY_TRACE` it now also accepts exactly `0x320`
for display 1 (a compare with the whole word, no mask, so other rotate, scale or boot-rotate bits are refused). Without the flag the gate's instructions
are the same as before (`boot_args_contract_test` hashes them), so the plain and release kernels keep their contract. `VIDEO` and the console rotation
are unchanged. Display, stride, width, height, base and the memory geometry are still checked exactly; the test refuses each altered value. xnu-12377 is untouched.
The draw target is unchanged: physical `0xF5A00000`, identity-mapped as Device by the stage 3 block before the MMU is on and by that same block after.

## Mac results (204662a7f, iPadOS 17)

All builds succeeded and the regression tests passed (8 of 8). The normal OLED and Erista packages took 76 actions,
`diag_state` and `diag_d1min` 19, `//boot:erista_first_xnu` 159, and `kernel_unprelinked` succeeded. The first-XNU package
has a 17543168-byte kernel window in 87 blocks, entry `0xb53cc4c8`, written to `bazel-bin/boot/erista-first-xnu`. The
linked loader margin was not reported and stays unmeasured.

## Mac results, 2026-11-26 (iPadOS 17)

The regression tests passed (24 of 24). The normal package and the first-XNU diagnostic package (58 actions) built, as did the
early-trace build: kernel window 17543168 bytes in 87 blocks, entry `0xb53cc4c8`. The loader margin, now measured with
`first_xnu_check.py margin`: `flat_bytes` 122880, `image_end` `0xa001e000`, `to_stamps_slot` `0xf00` (3840 bytes) and
`to_sd_reference` `0x1000` (4096 bytes).

`loader.uboot.bin` of `diag_state`, `diag_d1min` and `diag_loader_r` are one file,
`cdc52c651a0ae8a8e21d1932f63813702131dc0e91db7a1d2883890ebd59cd2a`. It differs from the Linux flat hash `c101d9c3...` because the
two are different artifacts: the Linux value is the flat image from `tools/package/diag_images.py` (clang 18.1.3), the Mac file adds
the U-Boot header and comes from Xcode's toolchain. At this commit and at `5f474ed0d` (the tree of the D0-R hardware run) the
Linux flat D0-R image is identical, and all seven pinned hashes still reproduce. No Mac hash of the earlier D0-R run is recorded
in this repository, so a Mac-to-Mac comparison needs a build of `5f474ed0d` on the Mac (`ERISTA_D0S_RUN.md`, section 1).

## Stops

| Condition | Action |
| --- | --- |
| A panic or exception | the screen stays (`reboot_s` 0); photograph it; hold POWER |
| No new mark for 30 s | hold POWER. The 30 s is provisional, like the loader's deadlines |
| `XNU VM INITIALIZED` shown and 120 s since the loader started | hold POWER. Provisional. No temperature limit is known: T2 gave one loader-time reading, T3 and T4 have not run |
| Anything unexpected on screen | hold POWER first, then read it |

Nothing resets a hang in this kernel: no watchdog is armed before userspace (`ERISTA_STOP_PATHS.md`), and a normal stop spins. The person
at the console is the only safety mechanism, so the run is never unattended. Before and after, Nyx's temperature readout is noted as
for T1 v3.

Where the boot ends is decided in the next section.

## The SD driver (N7 follow-up D)

Read from `NativeSwitchBlock.cpp`, nothing run.

**No root partition.** `setup()` reads the GPT and needs exactly one APFS or named partition. With none (or a bad GPT, or two) it logs and returns false,
so the device is never published and `firstSector` and `sectorCount` stay 0. The write gate at `command()` then refuses every write (`panic` before it
reaches the card). The kernel's root wait times out, calls `switch_raw_root_wait_diagnostic` ("not published") and panics `Failed to mount root device`
(`IOKitBSDInit.cpp`). How long the wait lasts was not read. The panic screen is the stop.

**Everything that writes to the card or its supply.**

| Path | Reached on the Erista when | Gate |
| --- | --- | --- |
| Block writes (CMD24/25), `issue()` and the queue paths | a client writes (APFS mount, `bsd_init`) | refused outside the APFS partition, `panic` otherwise |
| Extension registers 260 to 262 (cache, flush, CQ enable), CMD49 in `CqBus::control` | `checkCq` in `setup()`, when the card offers a performance function | only those bytes and values, anything else panics. Not behind a boot-arg |
| SD initialisation (CMD0 and on), LDO2 to 1.8 V, PLLC4, tap and trim | `raiseBusSpeed` | skipped by `HIDREV` major below 2 (the Erista's `00022117` gives 1), before any PMIC read. `switch-no-sd-uhs` skips it earlier |
| LDO2 off and on, card GPIO PE4, PMC SDMMC1 bits | system sleep only | no sleep without PMIC and sensors in this configuration (unverified) |
| Live proofs (writes read back) | after real writes | no synthetic write exists; the driver reads the partition only for its own checks |

The one unconditional card write is the CMD49 pair around `checkCq`: it changes the card's CQ and cache registers, never user data. It cannot be
turned off without a driver change (a boot-arg or a `DEVELOPMENT` flag). Not read: `SwitchCryptex.cpp` and the root-boot code in `bsd_init.c`.

**Storage-free configuration.** There is none today: the driver always runs `setup()` when the ADT node exists. Two ways to get less, neither built:

1. Leave the SDMMC1 node out of the first-XNU ADT. No device, no GPT read, no CMD49. The root wait times out and panics, so the stop is the same screen. Cost: it
   leaves the SD path unexercised, and the node's absence needs an ADT edit and `adt_check` change.
2. Keep the node and add `switch-no-sd-uhs` to the boot-args for the first-XNU package only. This changes one thing, the already-skipped 1.8 V path. It does not
   stop the CMD49 pair, so it is a second guard on a branch that the `HIDREV` test already skips.

**Ending the diagnostic at VM initialisation (N7-H, built, host-checked only).** The goal of the first boot is `arm_vm_init` returning. Two events are kept apart:

| Event | Evidence | Where |
| --- | --- | --- |
| Stage 21 | green mark 21 and `switch_early_record[1] = 21`; it is set before `switch_diag_early_leave`, the TTBR switch and the TLB flush | `arm_vm_init.c`, `SwitchDiag.c` |
| `arm_vm_init` returned | the text `XNU VM INITIALIZED` and `ARM_VM_INIT RETURNED AFTER STAGE 21` | `SwitchVmDone.c`, called right after `boot_progress_stage("arm_vm_init returned")` |

Mark 21 without the text means the kernel died between stage 21 and the return. The old HUD never runs in this configuration: with Video display 1 and
depth `0x320`, `switch_diag_init` takes the native-profile branch and leaves the framebuffer to the boot logo. `SwitchVmDone.c` maps the framebuffer with
`ml_io_map` (usable once `arm_vm_init` returned), clears a 130-column band that does not touch the marks at column 280, and draws the text. Nothing else
is mapped or written.

With `switch_stop_after_vm=<seconds>` in the boot-args it then ends the run with a timed reset, before `PE_init_platform`, IOKit and the SD driver.
`boot_args.py --stop-after-vm[=N]` writes the word (default 15, `vm_reset_seconds` in the package rule) and only `//boot:erista_first_xnu` and
`..._seeded` pass it (`stop_after_vm`, which needs `require_early_trace`). The normal packages' boot-args are unchanged. Release kernels link none of it
(`SwitchVmDone.c` is in `SwitchDiagDebug`, the call is an empty macro, `vm_done_source_test`).

**The timed reset.** Not a hang or temperature protection: it ends one observation, and 15 and 60 s are settings, not a safe duration. Sequence:

1. The word is read from the command line by `SwitchVmReset.h` (`vm_reset_test`). Bare means 15; `=N` means 1 to 60 in plain decimal. A value outside the range, text,
   an empty value, a number that would overflow, or the word twice is refused: the screen says `BAD SWITCH_STOP_AFTER_VM. NO RESET`, and the core parks. Absent means no stop at all.
2. It must be EL1 (`CurrentEL`), `cntfrq_el0` must be nonzero, `seconds x frequency` must not overflow and `cntpct_el0` must advance. Otherwise: `NO RESET`, park.
3. Interrupts are masked, `cntkctl_el1` gets the loader's event stream (EVNTEN, EVNTI 15, other bits kept), and the core waits in `wfe` until the counter passes the deadline.
4. One `smc #0` with PSCI `SYSTEM_RESET`. If it returns, the screen says `SMC RETURNED <x0>. NO RETRY` and the core parks. There is no second call.

The wait is the loader's own (`exit.c`), which returned to hekate from EL1 on this board. Not verified: the call after XNU owns the core (MMU on, its vectors), and that the event
stream wakes `wfe` under XNU's `cntkctl_el1`. If it does not, `wfe` never returns and only the power button ends the run; that stays an open item. The parked states use no timer and
do not spin, but nothing powers the console off. The panic path has no reset added.

Hardware boot stays held. It is not allowed until the owner approves the run itself, and the held power button stays the recovery. The row stays `boot 0`.

## Real-hardware preparation (N7 follow-up E)

Nothing here is approved. Each item is something the person at the console needs, or something a hold still blocks.

**Artifacts.** Built on the Mac, then compared. The image hashes of D0 to T1 v3 are in `boot/diag/erista-diag-image-hashes.txt`; D0-R's is
`c101d9c3ed21b8d62f1370e33871b857a41ffff64f8214092cece1d151b8105e`, a flat image built on Linux. D0-S and D1-min reuse D0-R's loader, so their Mac `loader.uboot.bin` files equal each other and the Mac D0-R build.
That file is a U-Boot image (a 64-byte header plus the flat image) from Xcode's clang, so it differs from the Linux value (see "Mac results, 2026-11-26").

```
bazel build --config=ios17 //boot:diag_state //boot:diag_d1min //boot:diag_loader_r
shasum -a 256 bazel-bin/boot/diag_state/loader.uboot.bin bazel-bin/boot/diag_d1min/loader.uboot.bin \
  bazel-bin/boot/diag_loader_r/loader.uboot.bin                    # three equal lines
shasum -a 256 bazel-bin/boot/diag_state/package/switchroot/*/* bazel-bin/boot/diag_d1min/package/switchroot/*/*
```

The package files are `bazel-bin/boot/diag_state/package` (entry `ios-diag-state`) and `bazel-bin/boot/diag_d1min/package` (entry `ios-diag-d1min`). Their
SHA-256 values were not reported yet, so none are written here; the Mac fills them in.

**U-Boot commands the two scripts use**, against the pinned `bl33.bin` (help text and strings only; none of this has run on an Erista unless marked):

| Command | In `bl33.bin` | Ran on the Erista |
| --- | --- | --- |
| `setenv`, `test`, `echoe`, `fdt`, `cp.b`, `bootm`, `itest.l`, `reset` | yes | yes (D0, D0-R) |
| `setexpr.l NAME *ADDR` | yes (the OLED's normal script uses it) | no |
| `setexpr NAME ${A} \& MASK` | the help text found names the load form and `sub`/`gsub`; the operator form was not found | no |
| `mmc dev 1`, `mmc read ADDR BLK CNT` | yes | `mmc dev` yes (`erista-sd-pages.log`); `mmc read` of this kind no |
| `crc32 ADDR COUNT` | yes (prints `CRC32 for ... ==>`) | no |
| `itest.w *ADDR == VALUE` | yes (`[.b, .w, .l, .s]`) | no |

D0-S depends on `setexpr` with `&`. A failed step names its status and resets, so a missing operator shows as a named failure and not a hang.

**Placement.** Copy `bootloader/ini/<entry>.ini` and `switchroot/<entry>/` from one package at a time onto the SD card's FAT, as `boot/diag/README.md`
describes; nothing existing is overwritten. Remove them after the run. D0-S first, D1-min only after its output is reviewed. The first-XNU package
is last and needs `boot` 1, which this tree does not set.

**Expected output.**

| Run | On screen | Pass |
| --- | --- | --- |
| D0-S | D0-R's report, then the framebuffer properties (address `F5A00000`, 720x1280, a8b8g8r8, rot 3) and two SDMMC1 words (`0x700b0024`, `0x700b002c`) | the same report lines D0-R printed, plus both words and the CAR reset and clock bits |
| D1-min | D0-R's report, `D1-MIN SECTOR 0 READ TO 0xA001F200`, a CRC32 line, then `SIGNATURE 55 AA PRESENT` or `ABSENT` | a CRC for 512 bytes, the loader still `CLEAR` for the SD reference range |
| First XNU | green marks 1 to 21 at y 280 (with the trace build), then `XNU VM INITIALIZED` and the stop text, or a panic screen | marks in order, a readable panic, no heat |

**Run time and stop conditions.** D0-S and D1-min end by themselves (15 s, then `reset`, as D0). Their basis is D0's own run. The first-XNU limits (30 s with no new mark, 120 s after
`XNU VM INITIALIZED`) are provisional: no run measured a stage time and no temperature limit is known. Hold POWER on any stop condition.

**T2 to T4.** T2 ran once (2026-11-26, `ERISTA_T2_RESULT.md`): the `0F` write form, BUSY and the ACKs at the changed divisor were observed once. Open and unchanged: T3's fan
and T4's protection design (`ERISTA_T2_DESIGN.md` has the loader's one-shot). The safety mechanisms that exist for a run are the person's power button and Nyx's temperature
before and after. A hang has no hardware reset (`ERISTA_STOP_PATHS.md`). None of the three is a reason to run, and the owner may waive them in writing.

## Before D0-S and D1-min (N7-J)

Both are read-only diagnostics over D0-R's loader. Neither is approved to run. Before either, all of:

| Check | How |
| --- | --- |
| The three loader images are equal | the `shasum` command above: `diag_state`, `diag_d1min` and `diag_loader_r` give three equal lines. Compare with the Linux flat hash only on Linux |
| The package holds only its own entry | `bootloader/ini/ios-diag-state.ini` (or `-d1min`) and `switchroot/ios-diag-state/` (or `-d1min`), nothing named `ios`, and the SHA-256 of each file recorded before copying |
| No iOS entry is bootable | the Erista row is `boot 0`; `iOS` is not in hekate's list for this card, or the normal package is absent |
| hekate is 6.5.5 and its entry list is photographed | the version the T1 v3 and D0 runs used |
| Temperature and battery noted | Nyx's readout before, written down before the run (D0-S v2's start was not recorded). Start only below the provisional 40 C candidate. Battery charged, console on a table, Joy-Con off |
| Someone is at the console and has read the stop rules | POWER held for any stop condition; the scripts end in `reset` after their dwell, "expected to land in hekate, not verified" |
| The expected screen is known | the table above. D1-min only after D0-S's photograph is reviewed |
| Nothing else touches the card | no write by either script. The card is not removed while the console is on |

After the run: photograph first, then remove the entry's files, then record Nyx's temperature. A screen that differs from the table is a finding to send, not to explain away.

**`setexpr` operators, from the pinned `bl33.bin`'s strings.** The help text there reads `setexpr[.b, .w, .l] name [*]value1 <op> [*]value2 ... <op> can be &, |, ^, +, -, *, /, %`, so `&` is built in, as are the `gsub` and `sub` forms. D0-S's two
`\&` lines would use it. Not established by strings: that hush passes `\&` through as a literal `&` (it is the one escaped character in the script), and that the result is printed as bare hex (`4000`, which
D0-S's `test` compares as text). If either fails, the line takes the `CAR BIT TEST FAILED` or `NOT READ` branch, which only skips the SDMMC1 reads. `itest` compares whole words and cannot isolate one bit, so it is not a substitute. No change is made.

## T2 in a hardware diagnostic (design)

Nothing here is built into an image. The sensor sequence exists on the host (`boot/diag/t2`, `t2_test`). A hardware run needs:

1. A loader image `diag_t2`: T1 v3's loader (its I2C1 prelude, gates, allowlist and stop rules, `ERISTA_T1V3_DESIGN.md`) with `t2.c` as the sensor stage. The bus layer implements `t2.h`'s single register transfers
   with T1 v3's transactions unchanged: divisor `00FF0001`, the same five I2C1 registers. U-Boot's script is T1 v3's with new text. It gets its own entry name and a new line in `erista-diag-image-hashes.txt`;
   the seven existing hashes do not move.
2. The allowlist is T1 v3's, extended in values only: `CMD_DATA1` pointers `03 11 12 02 01 10 00 15` and `0F`, `CNFG` `2802` and `2A02` (`ERISTA_T2_DESIGN.md`, "All writes"). `diag_mmio_audit_test` gets the same pairs and a count of one sensor write.
3. Stop codes `07xx`, the 15 s countdown and the 50 s deadline as T1 v3. The deadlines stay provisional.
4. Host tests before any build is run: `t2_test` against a model of the part (exists), an image inspection like `diag_loader_inspect` (the allowlist, no other store), and a script test like `diag_t1v3_test`.
5. The run order: D0-S, D1-min (if wanted), then T2. T2 has the sensor write, so it is last and needs its own approval. Nyx's temperature before and after, as for T1 v3.
6. What it will not settle: the conversion time's upper bound, which stays the 100 ms design bound, and whether the sensor ACKs other reads at that divisor in shutdown. Both are what the run is for.

## T3 and T4 as safety mechanisms (design)

None exists in code. What each would be, what proves it, and where it stops.

| Mechanism | Covers | Needs | Proved by | Status |
| --- | --- | --- | --- | --- |
| The person, POWER held | everything, slowly | nothing | the stop rules above | the only mechanism there is |
| Park, then reset by WDT0 after N seconds | a stop that ends by itself: VM done, panic | a WDT0 arm in `SwitchVmDone.c` (an MMIO write the panic path already makes elsewhere), a safe N, a reset that lands in hekate | one approved run; the OLED's panic path is the precedent, the Erista's is unverified (PMC scratch, whether the reset returns to hekate or loops) | not built, not approved |
| PSCI `SYSTEM_RESET` from EL1 | the same, without the PMC | a conduit: the ADT has no `/psci`, so either a node (ADT change) or a fixed `smc` in the diagnostic | a diagnostic that issues it once and lands in hekate | not built. U-Boot (EL2) got a return to hekate; EL1 after XNU is unverified |
| T3: fan, 5 V rail and PWM from hekate's sequence | heat while parked, a hang with the fan stopped | PWM clock already on (a CAR write is not part of T3), GPIO, pinmux, PMC writes (`regulator_5v_enable`) | tach edges: zero off, above zero at duty 51, up at the next step | designed (`ERISTA_THERMAL_DESIGN.md`); every write unapproved |
| T4: MAX77620 watchdog | a hang | I2C5 writes; the PMIC's state across its own restart | would have to show hekate not looping | rejected for now: a boot loop is possible and nothing shows otherwise |
| T4: SOC_THERM THERMTRIP | overheating | calibrated sensors, a driver's worth of register work, a PMC reset command the DTB does not give | cannot be tested by heating the console | out of reach; thresholds would be set by review |

**What the first XNU run can rely on, today:** the person, a 30 s no-progress limit, a 120 s total limit (both provisional), a parked CPU that draws less than a spin but stays hot, and Nyx's reading before and after. A parked
console cools only as far as the fan hekate left running. **Temperature protection:** the first-boot window is short (loader seconds, then XNU's VM init), the criteria are the provisional 40 and 55 C, and none is an official
limit. No mechanism here acts on a temperature by itself. **Hang protection:** none acts without a person. So the run is attended, and the hardware boot stays held until the owner either approves one of the first three rows
after its own proof or accepts the person as the stop in writing.

The call from XNU's EL1 and a timed stop are worked out in `ERISTA_PROTECTION.md`; nothing there is built.

## From now to the first XNU boot

`ERISTA_FIRST_XNU_BLOCKERS.md` restates this list after D1-min and assesses deferring T3, T4 and the full D1; the full D1 is not a condition of the first run. `ERISTA_FIRST_XNU_MAC.md` is the Mac's checklist for the package.

In order; each line blocks the next unless the owner waives it in writing.

| # | Step | Settled by | Hardware |
| --- | --- | --- | --- |
| 1 | Mac verifies the commands below on one commit, including the trace-kernel scan and the three loader hashes | the Mac | no |
| 2 | D0-S v1 hung (cause unknown); D0-S v2 run and passed 2026-11-26 (`ERISTA_D0S_V2_RESULT.md`); the framebuffer attributes and the four register values are recorded | a photograph | done, read-only |
| 3 | D1-min ran and passed 2026-11-26 (`ERISTA_D1MIN_RESULT.md`): the CRC32 equals the Mac's, `55 AA` present, the range holds | a photograph | done, read-only, one SD read |
| 4 | T2 ran and passed 2026-11-26 (`ERISTA_T2_RESULT.md`): one sensor write, 160 writes, 35 transfers, shutdown kept | a photograph | done, one sensor write |
| 5 | A stop path chosen: a proven reset (WDT0 or PSCI) or the person, in writing | the owner | the first two need a run |
| 6 | Seed decided: pinned (public, fine for observation) or the per-install seed | the owner | no |
| 7 | T3 and T4: built and proved, or waived | the owner | yes |
| 8 | `boot` 1 for the build under test only, set by the owner | the owner | no |
| 9 | The run itself: trace kernel, one core, stop after VM, stop rules | the person at the console | yes |

## Mac: everything together

One session, one commit. Bazel was not run here: the Python and g++ checks use stand-ins, so Starlark validity, runfiles and the BUILD labels below are the Mac's to show. A static pass here found every BUILD label's file or target present
and exported, and no `**kwargs` in a BUILD file.

```
bazel test --config=ios17 \
  //tools/package/test:boot_args_contract_test //tools/package/test:first_xnu_check_test \
  //tools/package/test:erista_seed_test //tools/package/test:erista_seeded_build_test \
  //tools/package/test:erista_package_test //tools/package/test:diag_state_test \
  //diagnostics/switch/test:all //boot/loader/test:carveout_gate_test //boot/diag/loader/test:all
bazel build --config=ios17 //boot:package //boot:erista_package //boot:diag_state //boot:diag_d1min //boot:diag_loader_r
bazel build --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true //boot:erista_first_xnu
bazel build --config=ios17 --//install:prebuilt=local //boot:erista_first_xnu                   # must fail: no early_trace
bazel build --config=ios17 --config=release --//install:prebuilt=local //boot:erista_first_xnu # must fail: release
bazel build --config=ios17 --//kernel:early_trace=true //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel <that kernel>                                   # exit 0
bazel build --config=ios17 //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel <that kernel>                                   # exit 1
python3 tools/package/first_xnu_check.py margin bazel-bin/boot/erista_first_xnu_work/loader.uboot.bin
shasum -a 256 bazel-bin/boot/diag_state/loader.uboot.bin bazel-bin/boot/diag_d1min/loader.uboot.bin bazel-bin/boot/diag_loader_r/loader.uboot.bin
python3 tools/package/erista_seed.py generate && python3 tools/package/erista_seeded_build.py build
python3 tools/package/erista_seeded_build.py scan && python3 tools/package/erista_seeded_build.py clean --seed
```

The two `must fail` builds are the check; their error text is worth sending. The plain package's `shasum` of `bazel-bin/boot/full-ram.adt` and the `OLED` package's action count (76) should not change.

## Before anyone runs it

All of these, none of which is done:

- The owner approves the run and changes the row's `boot` to 1 for the build under test only. This document does not.
- The Mac builds the target above and the loader's linked margin is reported (`ERISTA_XNU_HANDOFF.md` lists the Mac commands).
- The trace build is used: only it draws marks.
- A seed decision (`ERISTA_RANDOM_SEED.md`): the pinned seed is public, which does not matter for an observation boot.
- A stop path chosen (see the table above) and the stop procedure read: POWER held, then Nyx for temperature.
- T2 has run. A temperature before and after the first XNU run comes from Nyx; T2's reading was taken in the loader and says nothing about XNU's heat.

## Not covered

PSCI restart, the PMIC, sensors, fan control, Wi-Fi, the GPU and userspace. iOS 27 is not built or checked here.
