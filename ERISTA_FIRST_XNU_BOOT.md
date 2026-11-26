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

**One mismatch.** The packager's Video is display 1 with depth `0x320` (`loader_abi.py VIDEO`). start.s accepts depth 32
for display 1 and `0x320` for display 0 only, so on a package built from today's values the gate falls to
`Lswitch_early_disabled` and no mark is drawn, trace build or not. XNU's own decode reads the low byte as bits per pixel
(`pe_init.c`), so `0x320` is a valid depth word. The test records the mismatch instead of hiding it. Two ways to close
it, neither done: make start.s accept the low byte 32 for display 1 (this also enables early rendering on the OLED, whose
behaviour with it was not checked), or change `VIDEO` (changes the loader image hash and XNU's console rotation). Needs
a decision.

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

Where the boot ends is not decided. With an SD card that has no root partition the root mount should fail and panic, which stops it at a
screen to photograph and exercises the first storage access. What the SD block driver does on a card with no root has not been read.

## Before anyone runs it

All of these, none of which is done:

- The owner approves the run and changes the row's `boot` to 1 for the build under test only. This document does not.
- The Mac builds the target above and the loader's linked margin is reported (`ERISTA_XNU_HANDOFF.md` lists the Mac commands).
- The Erista's boot args are compared with start.s's contract.
- A seed decision (`ERISTA_RANDOM_SEED.md`): the pinned seed is public, which does not matter for an observation boot.
- A stop procedure the person has read: POWER held, then Nyx for temperature.
- T2 run, or the run waived, so that a temperature before and after is available.

## Not covered

PSCI restart, the PMIC, sensors, fan control, Wi-Fi, the GPU and userspace. iOS 27 is not built or checked here.
