# Erista: XNU early initialisation on a Cortex-A57, second reading

Source reading of 2026-11-27, after `ERISTA_XNU_BOOT_STAGES.md`. No code, image or allowlist changed; nothing run; `boot` 0, feature mask 0, first XNU run on HOLD. Paths are `kernel/xnu-10063.141.1` (iOS 17). Marks: **confirmed** (read in the file named), **inferred**, **unknown**.

## What the kernel is built as

The iOS 17 kernel is built with `-D ARM64_BOARD_CONFIG_BCM2837` (`config/recipes_development.bzl:14`), the generic ARMv8 bring-up board of XNU, not an Apple CPU family. **Confirmed.** `pexpert/pexpert/arm64/BCM2837.h` sets `ARM_ARCH_TIMER`, `__ARM_ARCH__ 8`, `__ARM_COHERENT_CACHE__ 1`, `__ARM64_PMAP_SUBPAGE_L1__ 1`, `NO_MONITOR`; `board_config.h:133-141` gives `MAX_L2_CLINE 6` (64 byte lines), `MAX_CPUS 8`, one cluster. The recipes add `XNU_NATIVE_PAN=0`. This board config is shared with the OLED build, so everything below is common to both boards unless it says otherwise.

Consequences read in the code:

- `APPLE_ARM64_ARCH_FAMILY`, `HAS_APPLE_PAC`, `HAS_CLUSTER` are not set by this config. `start.s` guards the Apple-only parts with them: the tunables include (`:36`), `MDSCR_EL1` setup and `APPLY_TUNABLES` (`:1529-1541`), `CPU_OVRD` (`:1544`), PAC key init (`:1464`). So none of the Apple implementation-defined registers is touched in `start.s`. **Confirmed for `start.s`**; the same guards for the rest of `osfmk/arm64` were not all traced (**unknown**).
- CPU number is taken from `MPIDR_EL1` bits 15:0 under `BCM2837` (`start.s:234-236`), so Aff1 is part of the physical id. The Erista's four A57 sit in one cluster (Aff1 0), so ids 0 to 3 (**inferred**; MPIDR was not read on the Erista).

## The STARTUP hooks up to `STARTUP_SUB_LOCKS` (stage 13)

`kernel_startup_bootstrap` runs the subsystems `TUNABLES`, `TIMEOUTS`, `LOCKS` in rank order (`startup.c:286`, enum at `startup.h:59-64`). The tree has 21 direct `STARTUP()` entries in these three subsystems; two are x86 only. The macro-generated ones (`TUNABLE()`, `MACHINE_TIMEOUT`, `LCK_*_DECLARE`) were not enumerated.

| Subsystem | Hook (source) | Touches |
| --- | --- | --- |
| TUNABLES | `stack_init` (`kern/stack.c:131`), `panic_init` (`kern/debug.c:455`, reads the kernel UUID and the `debug` boot-arg), `zone_leaks_init_zrecs`, `kasan_tbi_init_zrecs`, `zone_tunables_fixup` (`kern/zalloc.c`), `smr_cpu_checkin_init_min_interval_us` (`kern/smr.c:2008`), `csr_bootstrap` (`bsd/kern/kern_csr.c`), `parse_bsd_args` (`bsd/kern/bsd_init.c:1279`), `legacy_footprint_entitlement_mode_init` (`bsd/kern/kern_exec.c:3411`), `PE_init_kprintf_config` (`pexpert/arm/pe_kprintf.c:56`), `exclaves_check_sk` (`kern/exclaves_boot.c:150`, reads `SPTMArgs`; whether it is compiled here was not checked) | boot-args and kernel globals in DRAM |
| TIMEOUTS | `hw_lock_test_to_init` (`kern/locks.c:501`), `ml_init_lock_timeout` (`arm64/machine_routines.c:701`), `ml_io_init_timeouts` (`kern/machine.c:733`) | boot-args, the timebase from stage 9 |
| LOCKS | `lck_group_init` (`kern/lock_group.c:151`), `rw_lock_init` (`kern/lock_rw.c:277`), `lck_init_pv` (`arm/locks_arm.c:221`), `workload_config_setup` (`kern/workload_config.c:95`) | DRAM |

**Confirmed** for the hooks named: none reads or writes a device register in the code read. The timeout hooks convert nanoseconds with the timebase that `pe_identify_machine` took from the ADT (19.2 MHz in the Erista tree); a wrong timebase would scale every lock timeout (**inferred**). `ml_init_lock_timeout` defaults come from `MACHINE_TIMEOUT` values in timebase ticks (`machine_routines.c:107-112`).

Later subsystems (`KPRINTF` and after) run from `arm_init.c:567` and `kernel_bootstrap`; they are outside stage 13.

## `CPUFAMILY_UNKNOWN`

`cpuid_get_cpufamily` returns `CPUFAMILY_UNKNOWN` (0) for ARM implementor with a part not in its short list, which includes the Cortex-A57 (`osfmk/arm/cpuid.c:114-140`). **Confirmed.** Its callers in this tree are the commpage (`osfmk/arm/commpage/commpage.c:169-174`, written to `_COMM_PAGE_CPUFAMILY`) and the `hw.cpufamily` sysctl (`bsd/kern/kern_mib.c:433-443`). No kernel code branches on it; the effect is on user space, which can read `hw.cpufamily` or the commpage word. Which Apple frameworks or daemons do so, and what they do with 0, is **unknown** from these sources. The OLED build has the same value, so a problem would show there first; nothing in the repository records one. The kernel does not set the family from the ADT.

## Apple-specific registers and instructions

- Stages 1 to 6: none outside the guarded blocks above. The instructions `start.s` uses at EL1 are generic (`msr` to `VBAR_EL1`, `TCR_EL1`, `TTBR0/1_EL1`, `MAIR_EL1`, `SCTLR_EL1`, `CPACR_EL1`, `TPIDR_EL1`). **Confirmed** in the lines read.
- `start.s:1529-1541`: the Apple path (`MDSCR_EL1` TDCC, tunables from `MIDR_EL1`) is compiled out here.
- The loader's own checks that bear on this (`boot/loader/copy.S:55-72`): MIDR must be a Cortex-A57 (`0x410fd070` under the mask), `CPUECTLR_EL1` (`S3_1_c15_c2_1`) bit 6 (SMPEN) must already be set, `ID_AA64MMFR0_EL1` bits 31:28 (TGran4) must be 0 (4 KiB granule supported), `CTR_EL0` line size field 4. These were measured to hold or were refused (D0, D0-R, T1 v3). **Confirmed** for the checks; the measured MIDR and ISAR0 words are in `ERISTA_NATIVE_PORT_PLAN.md` row 7 and 14.
- Not read: Apple instructions in `osfmk/arm64` files other than `start.s` (for example `machine_routines_asm.s`, `cswitch.s`, `caches_asm.s`, `locore.s`) and what they do under the BCM2837 guards. **Unknown.** Because the OLED build already boots on the same core, an unguarded Apple instruction would have shown up there; the Erista would see the same code.

## Page tables on the A57

- The tree has a native 4 KiB path for a non-Apple family: `pmap.h:118-121` (`!defined(APPLE_ARM64_ARCH_FAMILY) && !__ARM_16K_PG__`, "Native 4 KiB", `BOOTSTRAP_TABLE_SIZE` for two roots and seven L2 pages), `proc_reg.h:818-826` (`TCR_TG0_GRANULE_SIZE` is 4 KiB without `__ARM_16K_PG__`), `proc_reg.h:943-956` and `:972-1000` (`T0SZ_BOOT` and `T1SZ_BOOT` 26 with `__ARM64_PMAP_SUBPAGE_L1__`, `TCR_EL1_BASE` with 40 bit IPS, outer-shareable write-back walks, `TBI0`, no PAC bits, no E0PD, 8 bit ASIDs). **Confirmed.**
- A Cortex-A57 has the 4 KiB granule and no 16 KiB granule, which is why the loader refuses a core that lacks TGran4 and says nothing about 16 KiB. Whether the final kernel defines `__ARM_16K_PG__` was not shown by a compile in this session: the recipes do not set it and the compiler may. **Unknown here**; the 4 KiB branch exists and the OLED boots with this tree, which is **inferred** evidence that the build is 4 KiB. A Mac check of the built kernel's `TCR_EL1` value or `PAGE_SIZE` would settle it.
- The framebuffer block entries of stage 3 use 2 MiB blocks at `0xf5a00000` and `0xf5c00000` with device attributes, which a 4 KiB granule allows at level 2 (**inferred**: `start.s` writes slots `0xd68` and `0xd70` of the bootstrap table).
- 40 bit IPS suits the A57's 40 bit physical range (**inferred**; `ID_AA64MMFR0_EL1.PARange` was not recorded here). Erista DRAM and MMIO addresses are below `0x2_0000_0000`.
- `__ARM_COHERENT_CACHE__ 1` tells XNU that device DMA is cache coherent. Whether the Tegra memory controller path to DRAM is coherent for the SD DMA and later clients is **unknown**; `NativeSwitchBlock` does its own handling (`SwitchAdma.h`, `SwitchClientCopy.h`; not read for this).

## From stage 21 to the VM-done stop

1. `arm_vm_init` returns (`arm_init.c:548`); `switch_diag_init(args)` and `switch_panic_save_init()` run (`:549-551`); `boot_progress_stage("arm_vm_init returned")`; `switch_diag_vm_done()` (`:553`). **Confirmed.**
2. `switch_diag_vm_done` (`diagnostics/switch/SwitchVmDone.c`, debug kernels only) returns at once unless stage 21 was recorded. It maps the framebuffer with `ml_io_map(0xF5A00000, ...)`, clears a band, draws `XNU VM INITIALIZED`, and without `switch_stop_after_vm` returns to `arm_init` (the kernel goes on).
3. With the boot-arg: `reset_after` checks `CurrentEL` is 4 (EL1h) and that `cntpct_el0` advances, masks DAIF, writes `CNTKCTL_EL1` (EVNTEN, EVNTI 15, the PL0 bits kept) and loops on `wfe` until the deadline, then `smc #0` with PSCI `SYSTEM_RESET` (`0x84000009`); on a return it draws the failure text and parks with `wfi` and every interrupt masked. **Confirmed.** The same event stream and `wfe` wait is what the loader's `exit.c:46-49` uses; it worked at EL1 in the loader on the Erista (T4-R and earlier runs). Under XNU's own `CNTKCTL_EL1` it is **unmeasured**, which `ERISTA_PROTECTION.md` already says.
4. Nothing between stage 21 and this point arms a watchdog or enables an interrupt. The stop is a firmware call, the same one measured from the loader.
5. It is a stop before `PE_init_platform(TRUE)`, serial, `machine_startup` and IOKit, so none of the next two sections is reached by the first run.

## Interrupt controller not found

`PE_init_platform(TRUE, ...)` calls `pe_arm_init_interrupts` (`pexpert/arm/pe_init.c:481-483`), which calls `pe_arm_map_interrupt_controller` (`pe_identify_machine.c:200-230`). It needs a node with `interrupt-controller` = `master` and one with `device_type` `timer`. The generated ADT has neither (its comment says so, `native_adt_common.py:261`), so it prints that it did not find them and returns 0, and `pe_arm_init_timer` (`:247`) is not called. **Confirmed** for the code path and the ADT.

Effect, **inferred**: on arm64 `pe_arm_init_timer` only registers empty callbacks (`empty_funcs`, `:255-264`) through `ml_init_timebase` (`machine_routines.c:900-915`), so skipping it leaves `rtclock_timebase_func` as zero, which is what registering would have written. `cpu.c:1044-1048` copies those zero handlers either way. The timer interrupt is taken from the architected timer by `start.s` and `ArmCpu`, not by this path. The GIC itself comes from `ArmGIC` and `ArmCpu` later (`ERISTA_IOKIT_DEPENDENCIES.md`). What would break if `ml_init_timebase` is not called at all was not traced beyond these lines.

## Serial and kprintf

`serial_init()` (`arm_init.c:566`, `pexpert/arm/pe_serial.c:792-843`) returns 0 if the SoC base is 0; with `PI3_UART` (set by the BCM2837 config) it looks for ADT nodes named `gpio` and `aux`. The Erista ADT has neither, so no serial device is registered (**confirmed** for the code and the generator). `PE_init_kprintf` then uses the native text console when `switch_native_text_console_active()` is true (`pe_kprintf.c:62-66`). A node named `gpio` or `aux` added to the ADT later would make XNU write to the Raspberry Pi 3 GPIO and AUX registers at Tegra addresses; the ADT audit test lists the allowed nodes, which guards that.

## Still unknown after this reading

- The `__ARM_16K_PG__` setting of the built kernel (a Mac check settles it).
- Apple instructions in `osfmk/arm64` beyond `start.s`, and every macro-generated STARTUP entry.
- What user space does with `hw.cpufamily` 0.
- The coherency of the Tegra DMA paths.
- `MPIDR_EL1`, `ID_AA64MMFR0_EL1.PARange` and the A57's `CPUECTLR` value on the Erista itself as XNU will see them.

## Sources

`config/recipes_development.bzl`, `pexpert/pexpert/arm64/BCM2837.h`, `board_config.h`, `osfmk/arm64/start.s`, `proc_reg.h`, `osfmk/arm/pmap/pmap.h`, `osfmk/arm/cpuid.c`, `osfmk/kern/startup.c`, `startup.h`, `pexpert/arm/pe_init.c`, `pe_identify_machine.c`, `pe_serial.c`, `pe_kprintf.c`, `osfmk/arm/arm_init.c`, `diagnostics/switch/SwitchVmDone.c`, `boot/loader/copy.S`, `boot/diag/loader/exit.c`, `tools/package/native_adt_common.py`.
