# T4-A: the reset paths, from the sources

Source investigation of 2026-11-27, offline only. No code, allowlist or image changed; no watchdog armed; no new MMIO write; T4-R not run again. Each statement is marked **confirmed** (read in a source named here), **inferred** (follows from confirmed facts, not seen) or **unknown**.

## Sources and their weight

| Source | What was read | Weight |
| --- | --- | --- |
| The pinned `bl31.bin` (SHA-256 `dea0ecd4...`, the same file the T4-R package puts on the card) | disassembled here with `llvm-objdump` from the raw file; addresses below are file offsets | the code the console runs. Names are mine; its source is not in this repository |
| `CTCaer/switch-l4t-atf`, branch `switch-v5-atf2.6`, `plat/nvidia/tegra/soc/t210/plat_psci_handlers.c` | `tegra_soc_prepare_system_reset` and `tegra_soc_prepare_system_off` | the source of the binary's family; which commit built the pinned file is not known (`boot/diag/README.md`) |
| hekate v6.5.5 (`bootloader/l4t/l4t.c`, `bootloader/main.c`, `bdk/soc/hw_init.c`, `bdk/soc/pmc_t210.h`) | the reboot-to-payload hand-off, the PMC bit names, what `hw_init` clears | the version on the console |
| Mainline Linux `drivers/watchdog/tegra_wdt.c`, `tegra210.dtsi` | register layout and write order; the T210 device tree | upstream |
| NVIDIA L4T `linux-4.4` `tegra_wdt.c` (a mirror) and the L4T 28.2 watchdog page | the same driver as shipped, timer 7 | vendor; not the 4.9 kernel Switchroot ships |
| The pinned `erista-sku0.dtb` | the `timer@60005000` and `watchdog@60005100` nodes | the device tree this repository boots with |
| This repository (`SwitchPanicSave.c`, `heartbeat.h`, `boot/diag/README.md`, `ERISTA_STOP_PATHS.md`) | the arm and stop sequences it already carries | this tree, never run on an Erista |

## The PSCI `SYSTEM_RESET` path (what the diagnostics use)

Confirmed in the binary and in the ATF source:

1. The SMC dispatcher indexes `0x84000000 + n` through a jump table. `n` = 9 goes to a short stub that calls the generic `psci_system_reset` (`0x591c`): it runs the SPD hook if one is set, flushes the console (`0x26c4`) and calls the platform `system_reset` pointer of the PSCI ops table (`0xb088`, offset `+0x50`), which points to `0x8348`.
2. `0x8348` calls `0x7e1c`, the `tegra_soc_prepare_system_reset` of the ATF source. In order:
   - CAR `0x60006028` (`SCLK_BURST_POLICY`) is written `0x10000000` and `0x60006030` is written 0 (the ATF source's two CAR writes), then a 1 ms delay;
   - `PMC_SCRATCH0` (`0x7000e450`) is read. If bit 1 (`PMC_SCRATCH0_MODE_RCM` in hekate) is set, or if HIDREV major (`0x70000804` bits 7:4) is 2, the payload step is skipped;
   - otherwise the reboot-to-payload function (`0x5d74`) runs, and then the PMC reset.
3. The reboot-to-payload function, from the binary: if the payload pointer in BL31's platform parameters (`+0x50`) is not zero, it clears 256 KiB of IRAM (`0x40000000`), copies the payload (size at `+0x48`) to IRAM `0x40010000`, copies 32 bytes of BL31's own RAM (`0xfff0f420`, zero-initialised in the file, so filled at run time by something not traced) to `0x40010094`, then writes: `PMC_SCRATCH0` bit 0 set (hekate names it `PMC_SCRATCH0_MODE_WARMBOOT`), `PMC_SCRATCH45` (`0x7000e634`) `0x2e38dfff`, `PMC_SCRATCH46` (`0x7000e638`) `0x6001dc28`, `PMC_SCRATCH33` (`0x7000e520`) `0x40010000`, `PMC_SCRATCH40` (`0x7000e53c`) `0x6000f208`, clears bit 13 of `PMC_CNTRL`, and clears bit 1 of `0x6000c200`.
4. The reset itself (`0x7160`): `PMC_CNTRL` (`0x7000e400`) bit 4 (`MAIN_RST`) is set, then `wfi`.

Confirmed from hekate v6.5.5: `launch_l4t` copies the relocator stub and hekate's image to `R2P_PAYLOAD_BASE` and passes its address and size to BL31 in `bl31_plat_params`; the stub's entry is `0x40010000` and the boot config follows it at `+0x94` (`PATCHED_RELOC_ENTRY`, `PATCHED_RELOC_SZ` in `main.c`). `0x40010000` and `+0x94` in the binary match those names.

Inferred: the boot ROM takes the warm boot path because `PMC_SCRATCH0` bit 0 is set, with the entry in `PMC_SCRATCH33`, and so runs the relocator, which puts hekate back. No source here describes the boot ROM's use of those scratch words, and the purpose of `PMC_SCRATCH45`, `46` and `40` is not known to me. What is confirmed is that the console went back to hekate every time (seven runs, `ERISTA_STOP_PATHS.md`, and T4-R).

The same BL31 ends `SYSTEM_OFF` in an error and `panic()` (ATF source; the binary's off stub falls through to the reset code).

## What the PSCI path needs, and what it leaves

- BL31 alive and the SMC working at EL1: shown by every run.
- hekate's copy at `R2P_PAYLOAD_BASE` intact at the moment of the reset. **Confirmed** in hekate v6.5.5 `bootloader/l4t/l4t.c` (`l4t_defs.h` could not be fetched; the defines are in this file): `SECFW_BASE` is `GEN_CARVEOUT_TOP` (`0xFFE00000`) and `R2P_PAYLOAD_BASE` is `SECFW_BASE + SZ_256K`, so `0xFFE40000`, inside the secure firmware carveout (`0xFFE00000` to `0xFFF00000`). hekate also zeroes `R2P_PAYLOAD_BASE + 0x94` (the boot config) there. Any later run that lets another program write that region could turn the return into a jump into garbage; the diagnostic loaders do not touch it, and T4-A's sequence must not either.
- The reset is `MAIN_RST` (`PMC_RST_STATUS` 3, "software main") with a boot ROM warm boot request. Hekate then saves and clears the reset status in `hw_init` (`hw_rst_reason`, `PMC_RST_STATUS` written to POR, `PMC_SCRATCH200` zeroed), so the cause is not readable by anything started from hekate.

## WDT0 expiry: how it differs

| | PSCI `SYSTEM_RESET` | WDT0 expiry with bit 15 (`PMC2CAR`) |
| --- | --- | --- |
| Who starts it | the loader's `smc` | the TKE hardware, after the count runs out (4th expiry per Linux's comment; the L4T node says `nvidia,expiry-count` 4) |
| BL31 runs | yes | no. The binary builds one address in the TKE range (`0x60005010`, `TMRUS`) and none for the watchdog registers; the ATF source has no watchdog code. Confirmed for constants built by `mov`/`movk` only |
| Reboot-to-payload prepared | yes (IRAM copy, `PMC_SCRATCH0` bit 0 and the scratch words) | no |
| Reset type | `MAIN_RST` by software | a PMC reset with cause "watchdog" (`PMC_RST_STATUS` 1 in hekate's table). Whether it resets the whole SoC the way `MAIN_RST` does is unknown here |
| Where the console starts after | hekate, by the steps above | **unknown**; see below |

## After WDT0 expiry: confirmed, inferred, unknown

- Confirmed: none of the reboot-to-payload preparation happens on that path, and BL31 is not involved. hekate's `hw_init` (which runs when hekate starts, not on this path) clears `PMC_SCRATCH190` bit 0 with the comment "Unset WDT_DURING_BR", which suggests a boot ROM watchdog exists on some cold boots. Nothing read here says whether one is armed on this console.
- Inferred: if the SoC resets, the boot ROM runs a cold boot, because `PMC_SCRATCH0` bit 0 is not set by anything on this path, and then starts the console's normal boot chain from the eMMC, or enters recovery mode if the usual conditions hold (no usable boot image, or a jig or a chip that forces it). The `PMC_SCRATCH` words BL31 wrote at some earlier reset may persist; that was not checked.
- Unknown: what the owner's console does at a cold start. If a modchip or a payload on the boot path starts hekate, the console may land in hekate; with only an RCM jig and a USB injector it may land in the stock or custom firmware (not hekate), and the person has to inject hekate again. Nothing in this repository says which. Also unknown: whether the TKE watchdog configuration survives the reset (see `ERISTA_T4A_DESIGN.md`), and whether the reset reaches the whole SoC.

`ERISTA_T4_DIAG_PLAN.md` and the first T4-A design expected the console to land in hekate. That expectation was taken from the PSCI path and does not carry to a watchdog reset. `ERISTA_T4A_DESIGN.md` is revised for it.

## The owner's environment (answered 2026-11-27)

Erista HAC-001, RCM-unpatched, no modchip as far as the owner knows, an RCM jig that works, USB APX detection confirmed, hekate started by payload injection from a Mac before, Auto Boot off, Switchroot Linux booted from another card. So from a cold start the owner can reach hekate through RCM. **Recovery when a watchdog is left armed is unverified.**

## Where the console may land after a WDT0 expiry

| Scenario | Basis | What the owner does |
| --- | --- | --- |
| BootROM cold boot, eMMC chain starts | **inferred**: no warm boot request is set on this path | cannot inject unless RCM is entered; with the jig, enter RCM from the off state |
| BootROM cold boot enters RCM by itself | **inferred**, only if its usual conditions hold | inject hekate |
| The console hangs or stays dark | **unknown** | see POWER hold below |
| The TKE configuration survives the reset and the counter runs again | **unknown** | the second expiry would repeat the reset; the sequence cannot be ended from software that has not started |
| A boot ROM watchdog (`WDT_DURING_BR`) is armed | **unknown** | none known |

## Recovery

- POWER hold: the repository's documents cite Nintendo's roughly 12 s force-off (`ERISTA_THERMAL_DESIGN.md`, `ERISTA_T1.md`, `boot/diag/README.md`, `sd_loader/RUN_CHECKLIST.md`), each saying it was not tried here. The Nintendo pages were not fetched. It is **not** treated as a certain recovery: whether it works with a watchdog armed, in a reset loop, or while the SoC is held in reset is unverified, and whether a power cut clears the TKE block is unverified.
- RCM entry with the jig, then payload injection: works from the owner's cold start (reported). With a watchdog left armed it is unverified, because the console must stay off long enough to enter RCM and the BootROM's behaviour with a running TKE counter is unknown.
- After a landing outside hekate the card written by the T4-R procedure is unchanged.
- BPMP: BL31 powers it off on the Erista when hekate hands it an SC7 entry firmware (`NOTES.md`), so in a loader stage the BPMP firmware is not running. Whether the PMC2CAR reset needs a live BPMP is unknown (item 9 of the design); the loader stages cannot depend on it either way.
- PSCI `SYSTEM_RESET` after an armed watchdog is the T4-P question and is not part of T4-A.

A rehearsal the owner can do without any image of ours is listed in the design (conditions before a real device run).

## What stays unsaid

The boot ROM's behaviour at a warm boot, the meaning of `PMC_SCRATCH33`, `40`, `45`, `46` as written by BL31, whether `0x6000c200` bit 1 is a CCPLEX or a BPMP control, and the real conditions of the PMC2CAR reset are not in any source read here. None is needed to say that the two paths differ.

The `nvidia,tegra-wdt-t21x` driver was not found in any source reachable here, and the Switchroot kernel's configuration (which watchdog driver is built) was not read.
