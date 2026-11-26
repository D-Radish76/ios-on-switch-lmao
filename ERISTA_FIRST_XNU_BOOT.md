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
| Kernel startup steps | from here the HUD (debug, one CPU) clears the screen and prints each `kernel_bootstrap` step, `PE_init_iokit` included |
| IOKit init | the HUD's `PE_init_iokit` line |
| First storage access | `switch_root_event` records (the SD driver runs on every board). Whether the HUD draws them was not checked |
| GIC init | no mark. The nearest line is `platform interrupts initialized` after `PE_init_platform(TRUE)`. Whether that covers the GIC distributor setup is not verified |

The HUD's clear wipes marks 1 to 21 once `arm_vm_init` has returned; its first line, `XNU VM INITIALIZED`, stands for all of them.

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

## Stops

| Condition | Action |
| --- | --- |
| A panic or exception | the screen stays (`reboot_s` 0); photograph it; hold POWER |
| No new mark for 30 s | hold POWER. The 30 s is provisional, like the loader's deadlines |
| `XNU VM INITIALIZED` shown and 120 s since the loader started | hold POWER. Provisional. No temperature limit is known: T2 to T4 have not run |
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

**Ending the diagnostic when VM initialisation completes (design).** The aim is to stop before IOKit, the SD driver and the root wait, with nothing to poll.

- In a `DEVELOPMENT || DEBUG` kernel, after the HUD prints `XNU VM INITIALIZED` (the line stands for stages 1 to 21), `switch_diag_early_leave`'s caller
  checks a boot-arg `switch-stop-after-vm` and then masks all interrupts and loops on `wfi`. No timer, no wake source, so the core idles instead of spinning.
- The only exit is the power button held. Release kernels do not carry the code (`DEVELOPMENT || DEBUG` gate), which `switch_diag_release_test` should check.
- The boot-arg belongs to the first-XNU package alone; `boot_args.py` gets a flag, so the normal package's boot-args and image hashes stay as they are.
- Not built: it touches `arm_init.c` and the boot-args tool, and approval covered offline host tests only.

## Real-hardware preparation (N7 follow-up E)

Nothing here is approved. Each item is something the person at the console needs, or something a hold still blocks.

**Artifacts.** Built on the Mac, then compared. The image hashes of D0 to T1 v3 are in `boot/diag/erista-diag-image-hashes.txt`; D0-R's is
`c101d9c3ed21b8d62f1370e33871b857a41ffff64f8214092cece1d151b8105e`. D0-S and D1-min reuse D0-R's loader, so their `loader.uboot.bin` must equal it.

```
bazel build --config=ios17 //boot:diag_state //boot:diag_d1min //boot:diag_loader_r
shasum -a 256 bazel-bin/boot/diag_state/loader.uboot.bin bazel-bin/boot/diag_d1min/loader.uboot.bin \
  bazel-bin/boot/diag_loader_r/loader.uboot.bin                    # three equal lines, the D0-R value above
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
| First XNU | green marks 1 to 21 at y 280 (with the trace build), then the HUD, then the panic screen | marks in order, a readable panic, no heat |

**Run time and stop conditions.** D0-S and D1-min end by themselves (15 s, then `reset`, as D0). Their basis is D0's own run. The first-XNU limits (30 s with no new mark, 120 s after
`XNU VM INITIALIZED`) are provisional: no run measured a stage time and no temperature limit is known. Hold POWER on any stop condition.

**T2 to T4.** Open and unchanged: T2's register `0F` form and offset registers (data sheet), BUSY and the ACK at the changed divisor (one T2 run), T3's fan
and T4's protection design (`ERISTA_T2_DESIGN.md` has the loader's one-shot). The safety mechanisms that exist for a run are the person's power button and Nyx's temperature
before and after. A hang has no hardware reset (`ERISTA_STOP_PATHS.md`). None of the three is a reason to run, and the owner may waive them in writing.

## Before anyone runs it

All of these, none of which is done:

- The owner approves the run and changes the row's `boot` to 1 for the build under test only. This document does not.
- The Mac builds the target above and the loader's linked margin is reported (`ERISTA_XNU_HANDOFF.md` lists the Mac commands).
- The trace build is used: only it draws marks.
- A seed decision (`ERISTA_RANDOM_SEED.md`): the pinned seed is public, which does not matter for an observation boot.
- A stop procedure the person has read: POWER held, then Nyx for temperature.
- T2 run, or the run waived, so that a temperature before and after is available.

## Not covered

PSCI restart, the PMIC, sensors, fan control, Wi-Fi, the GPU and userspace. iOS 27 is not built or checked here.
