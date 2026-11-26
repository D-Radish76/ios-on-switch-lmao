# First XNU: what the Mac verifies

Offline checklist. Nothing here approves a run: `boot` stays 0, the feature mask stays 0, and T2, T3, T4, the full D1 and every XNU run stay on HOLD.
D1 is not a condition of the first VM-stop diagnostic (owner's decision, 2026-11-26); T3 and T4 are not waived (that decision is not made).
Bazel was not run where this was written. Every command is the Mac's to run on one commit, and the outputs are the Mac's to report.

## 1. The package, and its separation from the normal ones

| Item | Check |
| --- | --- |
| Only one first-XNU package | `//boot:erista_first_xnu` (and `_seeded`): `manual`, `require_early_trace`, `stop_after_vm`, `smp = :single_core`, output directories `erista-first-xnu` and `erista-first-xnu-seeded` |
| It cannot be built without the trace kernel or in release | the two builds that must fail (section 6) |
| The normal packages are untouched | `//boot:package` and `//boot:erista_package` build with the action counts recorded (76 for the normal OLED and Erista packages) and the `shasum` of `bazel-bin/boot/full-ram.adt` equals the earlier value |
| The diagnostics are untouched | the nine Linux hashes of `diag_images.py`; the Mac's three equal `loader.uboot.bin` files (`diag_state`, `diag_d1min`, `diag_loader_r`) |
| `boot` 0 | the Erista row in `kits/SwitchBoard/SwitchBoard.h` reads `X(ERISTA, "erista", 0, 0, 0, 0, ...)`: `t210b01` 0, `sku` 0, `boot` 0, features 0; `//boot/loader/test:carveout_gate_test` passes: the loader refuses the Erista (`0x306`) and U-Boot's script reports `WRONG_BOARD`. The first run needs the owner to set the row to 1 for the build under test |

## 2. The trace kernel (`early_trace`)

```
bazel build --config=ios17 --//kernel:early_trace=true //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel <that kernel>      # exit 0
bazel build --config=ios17 //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel <that kernel>      # exit 1
```

Exit 0 only when `start.s` holds the trace sequence (marks 1 to 6 drawn); the plain kernel must give 1. `//tools/package/test:first_xnu_check_test`
covers the scan. The trace kernel is a debug-profile build (`DEVELOPMENT || DEBUG`); release kernels link none of it.

## 3. The VM stop and the timed reset

The run ends at `arm_vm_init` returning, with `switch_stop_after_vm=N` (default 15 s, 1 to 60) in the boot-args and a PSCI `SYSTEM_RESET` from EL1.

| Item | Check |
| --- | --- |
| The word and its limits | `//tools/package/test:boot_args_contract_test`, `//diagnostics/switch/test:vm_reset_test` (the Mac ran it; it passed at 62c68bb6a) |
| Only the first-XNU packages carry it | `bazel aquery //boot:erista_first_xnu --output=text \| grep -c -- '--stop-after-vm'` is 1 or more; the same for `//boot:erista_package` is 0 |
| Release kernels link none of it | `//diagnostics/switch/test:vm_done_source_test` |
| What is not shown by any host test | the call after XNU owns the core; `wfe` waking under XNU's `cntkctl_el1`; whether hekate returns. These are what the run is for |

## 4. One core

The Erista ADT has one CPU and no PSCI node, and the package passes `smp = :single_core`.

`bazel test //tools/package/test:erista_package_test //tools/package/test:boot_args_contract_test //tools/package/test:adt_check_test`: the first two cover the package and its boot-args, the third the ADT audit (`adt_check.py`). The boot-args carry no `--smp`. A four-core build would show a different ADT CPU count and fail the package test.

## 5. Loader size and memory regions

```
python3 tools/package/first_xnu_check.py margin bazel-bin/boot/erista_first_xnu_work/loader.uboot.bin
```

Last measured (2026-11-26): `flat_bytes` 122880, `image_end` `0xa001e000`, `to_stamps_slot` `0xf00` (3840 bytes), `to_sd_reference` `0x1000`.
The loader must end below `0xA001EF00` (U-Boot's stamps slot) and `0xA001F000` (the SD reference page, `0xA001F000` to `0xA0020000`); `link.ld` asserts
the first. Report the new figures; a margin that shrank is a finding. The diagnostic loaders sit in the same window and D1-min showed the range clear
with a sector read at `0xA001F200`, so the SD reference page is not available to the kernel package either.

## 6. The seed and its secrecy

The pinned seed in `boot/BUILD.bazel` is public and the same on every console; for an observation boot that is accepted (`ERISTA_RANDOM_SEED.md`).
The per-install seed:

```
python3 tools/package/erista_seed.py generate        # 256 bytes to local/erista-seed.hex, mode 0600, git-ignored, never printed
bazel build --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true //boot:erista_first_xnu_seeded
python3 tools/package/erista_seeded_build.py scan    # nothing of the seed is in the tracked tree or the shared caches
python3 tools/package/erista_seeded_build.py clean --seed
```

Rules: the seed file and any seeded package are never committed, uploaded or put in a remote cache (`private_seed` marks the actions `no-cache` and
`no-remote`); only the 8-digit fingerprint may be written in a report. `//tools/package/test:erista_seed_test` and `erista_seeded_build_test` cover the
tool and the tags. Which seed the first run uses is the owner's decision (row 6 of `ERISTA_FIRST_XNU_BLOCKERS.md`).

## 7. The full command set

```
bazel test --config=ios17 //tools/package/test:boot_args_contract_test //tools/package/test:first_xnu_check_test \
  //tools/package/test:erista_seed_test //tools/package/test:erista_seeded_build_test //tools/package/test:erista_package_test \
  //tools/package/test:diag_state_test //boot/loader/test:carveout_gate_test //boot/diag/loader/test:all //diagnostics/switch/test:all
bazel build --config=ios17 //boot:package //boot:erista_package //boot:diag_state //boot:diag_d1min //boot:diag_loader_r //boot:diag_t2
bazel build --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true //boot:erista_first_xnu
bazel build --config=ios17 --//install:prebuilt=local //boot:erista_first_xnu                     # must fail: no early_trace
bazel build --config=ios17 --config=release --//install:prebuilt=local //boot:erista_first_xnu    # must fail: release
```

The two `must fail` builds are the check; their error text is worth sending. Report the action counts, the kernel window size (17543168 bytes in 87
blocks, entry `0xb53cc4c8` at the last report), the margin, and every hash. Building the package is not running it.
