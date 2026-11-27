# Erista: the early trace stages 1 to 21

Source reading of 2026-11-27. No code, image or allowlist changed; nothing run; the Erista row stays `boot 0`, feature mask 0, and the first XNU run stays on HOLD. Line numbers are `kernel/xnu-10063.141.1` (iOS 17). The iOS 27 tree (`xnu-12377`) was not compared here.

Marks: **confirmed** (read in the file named), **inferred** (follows from confirmed facts), **unknown** (not read, or not knowable without the console). A function body I did not open is marked "not read".

## How the marks work

Each stage draws a 16 by 16 green block at y 280, x `16 + 40 * (stage - 1)` into the framebuffer at physical `0xF5A00000` and records the stage in `switch_early_record[1]`. Stages 1 to 6 come from `SWITCH_EARLY_MARK` in `osfmk/arm64/start.s`, 7 to 21 from `switch_diag_early_stage` (`diagnostics/switch/SwitchDiag.c:71`), which refuses stages below 7 and above 21. Only trace builds draw (`--//kernel:early_trace=true`, debug profile); release kernels never do (`ERISTA_FIRST_XNU_BOOT.md`). A fault replaces the sequence with the early exception screen whose LAST STAGE line names the last stage reached.

Every mark writes ordinary DRAM (the framebuffer lies in the loader's reservation `0xF5A00000..0xF5E00000`, `ERISTA_NATIVE_PORT_PLAN.md` row 5). Marks touch no device register. **Confirmed** for `early_trace_mark` (`SwitchDiag.c:53`, `switch_diag_pixel` on `SWITCH_FB_PA`).

## The stages

CPU registers are the ones the stage reads or writes in the code named. "MMIO" means a device register; DRAM and the framebuffer are not counted.

| # | What happens | Function and source | MMIO | CPU registers | Needs |
| --- | --- | --- | --- | --- | --- |
| 1 | Video boot args accepted (depth `0x320`, else the trace stays off); the trace record is armed | `start.s:1086-1097` (`Lswitch_early_video_ready`) | none | reads boot args at `x20+0x50`; `x16`, `x17`, `x28` | the loader's boot args with a valid video block, MMU off, the framebuffer at `0xF5A00000` physical |
| 2 | Interrupts masked, low reset vector set, kernel memory parameters read from the boot args | `start.s:1107-1127` (`DAIFSet`, `MSR_VBAR_EL1_X0`, `BA_VIRT_BASE`, `BA_PHYS_BASE`, `BA_MEM_SIZE`, `BA_BOOT_FLAGS`) | none | `DAIF`, `VBAR_EL1`; `x22` to `x26` | stage 1; the loader's EL1h, DAIF-masked handoff (measured on the Erista, `ERISTA_NATIVE_PORT_PLAN.md` row 7) |
| 3 | Two 2 MiB device-attribute block entries for `0xF5A00000` and `0xF5C00000` written into the bootstrap page tables, so the framebuffer stays mapped when the MMU turns on | `start.s:1285-1305` (`Lswitch_fb_map_*`) | none | `x16`, `x17`, `x25` (table base) | stage 2; a free table slot (`0xd68`, `0xd70`, both must be zero or the stage branches to a failure label) |
| 4 | Vector table, PAC keys if `HAS_APPLE_PAC` (inferred absent: a Cortex-A57 has no PAC), then the MMU, caches and translation registers are set | `start.s:1460-1468`, then the `MSR_TCR_EL1_X1`, `MSR_TTBR1_EL1_X0`, `MSR_SCTLR_EL1_X0` macros (`start.s:53-95`) | none | `VBAR_EL1`, `TCR_EL1`, `TTBR0/1_EL1`, `MAIR_EL1`, `SCTLR_EL1` | stage 3. The values come from the Apple tree; whether they suit the A57 in every detail is **unknown** until it runs |
| 5 | MMU is on; vectors move to the kernel-virtual alias | `start.s:1495-1504` (`Lswitch_secondary_mmu_live`, `Lswitch_vectors_live`) | none | `VBAR_EL1`, `SCTLR_EL1` check | stage 4 completing; the identity map of the framebuffer from stage 3 |
| 6 | Tunables applied if the build has them, warm-reset test, then the call into C | `start.s:1554-1565` (`APPLY_TUNABLES`, `cbnz x21`) | none | `x21` (warm reset flag), `CPU_OVRD` only if `HAS_CLUSTER` (inferred absent) | stage 5 |
| 7 | `arm_init` entered | `osfmk/arm/arm_init.c:350`, mark at `:360` | none | stack pointer from `start.s` | stage 6 |
| 8 | Kernel image rebased and signed (`arm_slide_rebase_and_sign_image`) | `arm_init.c:362-364` | none | none (body not read) | stage 7 |
| 9 | Platform expert set up: boot args and video fields copied, the device tree initialised (`SecureDTInit`), `pe_identify_machine` reads `/arm-io` `ranges` (SoC base) and the running CPU's `timebase-frequency` and `bus-frequency`, then `/chosen` strings and `pe_init_debug` | `arm_init.c:366-378`; `pexpert/arm/pe_init.c:447-540`; `pexpert/arm/pe_identify_machine.c:35-100` | none: the device tree lives in DRAM. **Confirmed** in the lines read | none | `/arm-io ranges`: if the SoC base is 0 `pe_identify_machine` returns early and the clock defaults stay (24 MHz timebase); the Erista tree sets `ranges` from the GIC bases (`native_adt_common.py:252`) and the CPU `timebase-frequency` 19.2 MHz (`:165`) |
| 10 | CPU topology from `/cpus` (`ml_parse_cpu_topology`) | `osfmk/arm64/machine_routines.c:998`, mark `arm_init.c:451` | none (ADT only, confirmed in the part read) | none | one `/cpus/cpu0` with `reg`, `state` and timebase; single-core ADT for the first boot |
| 11 | Bootstrap thread and CPU data (`thread_bootstrap`, `cpu_bootstrap`), current thread set | `arm_init.c:459-481` | none | `TPIDR_EL1` through `machine_set_current_thread` (**inferred**) | stage 10 |
| 12 | Early clock: registers the timebase callback only | `osfmk/arm/rtclock.c:85` (`rtclock_early_init`), mark `arm_init.c:484` | none | none here (the callback validates `timebase_num/den` and panics on a bad pair, `rtclock.c:106-114`) | the ADT timebase (19.2 MHz on the Erista, `CNTFRQ` measured) |
| 13 | `kernel_startup_bootstrap`: sorts the `STARTUP` entries and runs the subsystems up to `STARTUP_SUB_LOCKS` (tunables, timeouts, locks) | `arm_init.c:486-488`; `osfmk/kern/startup.c:264-287` | none in the function itself; each startup hook is its own code (not read) | none known | stage 12; a non-empty startup section, else it panics |
| 14 | `cpu_init`: per-CPU timer queue, then `do_cpuid`, `do_mvfpid`, `do_cacheid`, `do_debugid`, CPU subtype | `osfmk/arm64/cpu.c:734-790`, mark `arm_init.c:496`; `osfmk/arm/cpuid.c:90` | none. **Confirmed** | reads `MIDR_EL1` (`machine_read_midr`), cache and debug ID registers; the CPU subtype comes from a compile-time arch, so an A57 cannot hit the `Unknown CPU subtype` panic (`ARMv8E` only with `HAS_APPLE_PAC`) | stage 13. `cpuid_get_cpufamily` maps a Cortex-A57 to `CPUFAMILY_UNKNOWN` (`cpuid.c:114-140`); the OLED's T210B01 is also an A57, so this is not an Erista difference, and which later code reads the family was not traced |
| 15 | `processor_bootstrap` | `arm_init.c:497-499` | none known (not read) | none known | stage 14 |
| 16 | `arm_vm_init` entered, boot args and memory sizes known | `arm_init.c:547-548`; `osfmk/arm64/arm_vm_init.c` | none | none | the DRAM banks: `0x80000000 + 0x7FD00000` and `0x100000000 + 0x80000000` (measured) |
| 17 | Memory bounds computed, physical aperture base chosen | `arm_vm_init.c:1876-1882` | none | none | stage 16; boot args `memSize`, `topOfKernelData` |
| 18 | Physical aperture slide from `early_random()` | `arm_vm_init.c:1884-1888` | none expected | none | a random seed in `/chosen`; the pinned seed repeats every Erista boot (`ERISTA_ADT.md` Open, `ERISTA_RANDOM_SEED.md`) |
| 19 | Final kernel page tables being built (`arm_vm_physmap_init`) | `arm_vm_init.c:2159-2160` | none | none | stage 18 |
| 20 | New kernel tables made live | `arm_vm_init.c:2159-2162` (`set_mmu_ttb_alternate`) | none | `TTBR1_EL1` alias | stage 19. The framebuffer and the early diagnostics must stay reachable until stage 21 |
| 21 | Early diagnostics retired, TTBR switched to the invalid table, TLB flushed, `kva_active` set | `arm_vm_init.c:2168-2175` (`switch_diag_early_leave`, `set_mmu_ttb`, `flush_mmu_tlb`) | none | `TTBR0_EL1`, TLB | stage 20. After this the marks stop; `SwitchVmDone.c` maps the framebuffer again with `ml_io_map` |

Stage 21 is set before `arm_vm_init` returns. Whether it returned is shown by the text `XNU VM INITIALIZED` (`SwitchVmDone.c`, called after `boot_progress_stage("arm_vm_init returned")`, `arm_init.c:551-553`).

## What the stages do not show

- **No stage has run on an Erista.** The first XNU run is on HOLD. Everything above is read, not observed, and the host tests check the trace code, not the console.
- Stages 7 to 15 have no mark of their own for their MMIO because they have almost none; the trace cannot tell a fault in a device register from a fault in memory. Stage 9 and 13 bodies are not read here.
- A mark is a DRAM write. If the display scan-out or the framebuffer mapping fails, a stage can pass and show nothing. The marks say "reached" only when the person sees them.
- Nothing in stages 1 to 21 arms a watchdog, enables an interrupt or starts a second core. **Confirmed** for `start.s` and `arm_vm_init.c` in the lines read; not for `kernel_startup_bootstrap` or `cpu_init` bodies.

## After stage 21 (to the end of the planned first run)

| Step | Function and source | State |
| --- | --- | --- |
| `arm_vm_init` returns, panic-save init, the VM-done text | `arm_init.c:548-553` (`switch_diag_init`, `switch_panic_save_init`, `switch_diag_vm_done`) | text drawn by `SwitchVmDone.c`; with `switch_stop_after_vm=<n>` in the boot args it then ends with a timed PSCI `SYSTEM_RESET` before `PE_init_platform(TRUE)`, IOKit and the SD driver (`ERISTA_FIRST_XNU_BOOT.md`). Built and host-checked only |
| `PE_create_console`, `PE_init_printf`, interrupt platform (`PE_init_platform(TRUE, &BootCpuData)`) | `arm_init.c:600-621`; `pe_init.c:481-483`; `pe_identify_machine.c:200-245` | `pe_arm_map_interrupt_controller` looks for a node with `interrupt-controller` = `master` and a `device_type` `timer`; the generated ADT has neither (`native_adt_common.py:261`), so it prints that it found none and returns 0, and `pe_arm_init_timer` is not reached. **Inferred harmless**: on arm64 that function only passes empty timebase callbacks (`empty_funcs`), so `ml_init_timebase` registers nothing either way. `ArmGIC` and `ArmCpu` bring up the GIC and the virtual timer later; read only on the Erista, no interrupt taken |
| `machine_startup`, `kernel_bootstrap`, `kernel_bootstrap_thread` | `arm_init.c:688`, `osfmk/kern/startup.c:696-939` | next document |
| `PE_init_iokit`, then `bsd_init` | `startup.c:820-821`, `:939` | see `ERISTA_IOKIT_DEPENDENCIES.md` |

## Erista against Mariko in these stages

The reference board of this repository is the OLED (`SWITCH_BOARDS` row `OLED`: `t210b01` 1, sku 3, boot 1, all features). The Erista row has `t210b01` 0, sku 0, `boot` 0, features 0 (`kits/SwitchBoard/SwitchBoard.h:47-50`). Both have four Cortex-A57, GIC-400 at the same bases and the 19.2 MHz architected timer (`ERISTA_NATIVE_PORT_PLAN.md` A-2, A-3).

| Area | Erista (T210) | Mariko (T210B01) | Effect on stages 1 to 21 |
| --- | --- | --- | --- |
| CPU, MMU, GIC, timer | same | same | none found: `start.s` and `arm_vm_init.c` contain no board test in the lines read (**inferred** for the rest; the plan notes the same, `ERISTA_NATIVE_PORT_PLAN.md` A-2) |
| `HIDREV` major (`0x70000804` bits 7:4) | 1 (`00022117`) | 2 | read in BL31 (`tegra_soc_prepare_system_reset` skips the reboot-to-payload when major is 2, `ERISTA_T4A_RESET_PATH.md`), the SD driver (`NativeSwitchBlock.cpp:3715`), the DFLL (`TegraDFLL/platform.c:695`) and `SwitchDisplayMemoryMath.h:50`; none is reached before stage 21 |
| DRAM and carveouts | banks and 25 MC words measured equal to the OLED's | | none; layout from `native_xnu_layout.py` is board independent on the evidence so far |
| Security engines | SE only | SE and SE2 | the loader keeps the pinned seed on the Erista (`loader.c:537`); stage 18 then slides from a repeating seed |
| ADT | `native_erista_adt.py`, minimal, audited by a table test | OLED tree | the Erista tree has one CPU and no `/psci`; it has not been checked by a real boot (**unknown**) |

## What is missing for the Erista before these stages can run

1. **The run itself.** Blockers 7 to 10 of `ERISTA_FIRST_XNU_BLOCKERS.md`: T3 and T4 built or waived by the owner, the build flag that sets `boot` 1 for the test build only, the run condition, the approval. None is done.
2. **Stage 3's assumption** that the framebuffer is at `0xF5A00000`, 720x1280, `a8b8g8r8` was passed by the loader's control flow but never printed (`ERISTA_NATIVE_PORT_PLAN.md` row 4).
3. **The loader step's open reads**: rows 16 and 18 (SD reference path, SD engine idle) have no Erista measurement.
4. **The A57's clock and voltage at the handoff** are unknown; XNU does no DVFS here (`ERISTA_NATIVE_PORT_PLAN.md` A-2).
5. **Seed**: stage 18 uses a seed that repeats on every Erista boot; the owner's per-install preference exists but no seeded package that can boot does (`ERISTA_FIRST_XNU_BLOCKERS.md` step 6).
6. **Hang protection**: nothing resets a hang before userspace; the person at the console is the only mechanism (`ERISTA_FIRST_XNU_BOOT.md`). The T4-A study does not change that yet.

## Sources

`kernel/xnu-10063.141.1/osfmk/arm64/start.s`, `osfmk/arm/arm_init.c`, `osfmk/arm64/arm_vm_init.c`, `osfmk/arm/rtclock.c`, `osfmk/arm64/cpu.c`, `osfmk/arm64/machine_routines.c`, `diagnostics/switch/SwitchDiag.c`, `SwitchVmDone.c`; `ERISTA_FIRST_XNU_BOOT.md`, `ERISTA_NATIVE_PORT_PLAN.md`, `ERISTA_ADT.md`, `ERISTA_FIRST_XNU_BLOCKERS.md`.
