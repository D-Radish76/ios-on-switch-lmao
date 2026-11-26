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
| The diagnostics are untouched | the nine Linux hashes of `diag_images.py`; the Mac's three equal `loader.uboot.bin` files (`diag_state`, `diag_d1min`, `diag_loader_r`); `diag_t2`'s Mac `loader.uboot.bin` stays `ce46a5d2...cba` (`boot/diag/erista-t2-run1.txt`) |
| `boot` 0 | the Erista row in `kits/SwitchBoard/SwitchBoard.h` reads `X(ERISTA, "erista", 0, 0, 0, 0, ...)`: `t210b01` 0, `sku` 0, `boot` 0, features 0; `//boot/loader/test:carveout_gate_test` passes: the loader refuses the Erista (`0x306`) and U-Boot's script reports `WRONG_BOARD`. A build that boots the Erista exists only behind `--//boot:erista_boot_run`, default false and not approved: `ERISTA_FIRST_XNU_BOOT_GATE.md`, section 7 for the analysis-only checks |

## 2. The trace kernel (`early_trace`)

```
bazel build --config=ios17 --//kernel:early_trace=true //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel bazel-bin/kernel/kernel.macho      # exit 0
bazel build --config=ios17 //kernel:prelinked
python3 tools/package/first_xnu_check.py kernel bazel-bin/kernel/kernel.macho      # exit 1
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

The owner prefers the per-install seed for the first XNU run too (2026-11-26): the package is `//boot:erista_first_xnu_seeded`. The pinned seed in `boot/BUILD.bazel` is public and
the same on every console; it stays the fallback for a build that is not for a card (`ERISTA_RANDOM_SEED.md`).

Run these on the Mac, in this order. The wrapper is the only way to build the seeded package: it adds `--config=ios17`, `--//install:prebuilt=local`,
`--//kernel:early_trace=true` and every cache flag, refuses a seed git could pick up, and refuses flags that write build details to a file (`-s`, `--profile`, build-event and execution-log files).

```
python3 tools/package/erista_seed.py generate           # writes local/erista-seed.hex (mode 0600, git-ignored); prints the fingerprint only
python3 tools/package/erista_seed.py check              # exit 0 only for a set, private, non-pinned seed
python3 tools/package/erista_seeded_build.py build      # the one build command
python3 tools/package/erista_seeded_build.py scan       # the seed is not in git's tracked files; add --also LOG for any saved output
shasum -a 256 bazel-bin/boot/erista-first-xnu-seeded/switchroot/*/*    # then copy to the card and compare, as for T2
python3 tools/package/erista_seeded_build.py clean      # after the card's copy is verified: removes the seeded outputs
python3 tools/package/erista_seeded_build.py clean --seed   # after the run is recorded, when no rebuild is planned
```

Why that order: `scan` runs right after the build and again before anything is committed or posted, because it reads git's tracked files and the files named with `--also`, nothing else, unless `--caches` adds the disk caches and bazel's own state (section 12). `clean`
removes the package, so it comes after the copy; the seed file goes last because a rebuild with a new seed gives a different package and the card's copy holds this one. `scan` does not look inside `bazel-bin`
or the card, which hold the seed by design: the window on the card contains it, so the card is not shared and the entry's files are removed after the run.

The seeded target carries the `no-cache` and `no-remote` tags, and the seed file is git-ignored, so it is never committed.

Never: commit, upload, paste or log the seed; put the seeded package or its logs in a remote or disk cache; pass the seed on a command line (bazel gets only the path `//local:erista-seed.hex`). A report carries
the 8-digit fingerprint and file hashes, nothing derived from the seed's bytes. `//tools/package/test:erista_seed_test` and `erista_seeded_build_test` cover the tool and the cache tags.
Final review of the seed path (2026-11-27), from `erista_seed.py`, `erista_seeded_build.py` and `boot/BUILD.bazel`:

| Check | Result |
| --- | --- |
| Where it runs | from the repository root: the seed path and `git check-ignore` are relative to the working directory |
| The file | 256 bytes of hex, mode 0600, created with `O_EXCL` unless `--force`; `local/.gitignore` names it; `load` refuses a group or world readable file, the pinned seed (whole or its first 64 bytes) and a seed with fewer than 100 distinct byte values |
| What the build gets | only the path `//local:erista-seed.hex`; `erista_adt_seeded` is `no-cache` and `no-remote`, and the wrapper adds `--remote_cache=`, `--remote_executor=`, `--noremote_accept_cached`, `--noremote_upload_local_results`, `--disk_cache=` |
| What the wrapper refuses | `-s`, `--subcommands`, `--verbose_failures`, `--sandbox_debug`, `--explain`, build-event, profile and execution-log flags |
| What it prints | the 8 digit fingerprint and "seed ok"; never the seed |
| What the package is today | the wrapper does not pass `--//boot:erista_boot_run`, so `//boot:erista_first_xnu_seeded` is built with the real board table: a package whose loader stops at `0x306` and whose script says `WRONG_BOARD`. It checks the seed path, the hashes and the copy to a card; it cannot boot the Erista |
| Closed (2026-11-27) | the wrapper used to pass extra arguments after `--` to bazel, so `-- --//boot:erista_boot_run=true` would have built the boot=1 seeded package. Now: the flag is refused in any spelling, `--config`, `--bazelrc`, `--flagfile`, `--flag_alias` and `@file` are refused, `.bazelrc`, `user.bazelrc` and `~/.bazelrc` naming it stop the build, `--//boot:erista_boot_run=false` is the last flag of every command, and an analysis-only `bazel aquery` of the same flags must list `BootScript` and no `BoardHeader` or `BoardAgree` before bazel builds anything. A future approval of the flag needs its own design |
| Left on the Mac after a build | the seed file, `bazel-bin/boot/erista-full-ram-seeded.adt`, the `erista_first_xnu_seeded_work` tree; `scan` reads git's tracked files and `--also` files only, so it does not look in those unless `--caches` names them (section 12) |

Which seed the run uses is recorded in row 6 of `ERISTA_FIRST_XNU_BLOCKERS.md`; the owner's written decision of the run itself is still open.

## 7. The full command set

```
bazel test --config=ios17 \
  //tools/package/test:boot_args_contract_test //tools/package/test:first_xnu_check_test \
  //tools/package/test:erista_seed_test //tools/package/test:erista_seeded_build_test //tools/package/test:erista_package_test \
  //tools/package/test:board_header_test //tools/package/test:boot_gate_rule_test //tools/package/test:boot_script_board_test \
  //tools/package/test:diag_boot_test //tools/package/test:diag_loader_test //tools/package/test:diag_memory_test \
  //tools/package/test:diag_sd_test //tools/package/test:diag_sd_loader_test //tools/package/test:diag_state_test \
  //tools/package/test:diag_state_audit_test //tools/package/test:diag_state_v2_test //tools/package/test:diag_s2_measured_test \
  //tools/package/test:diag_t1_test //tools/package/test:diag_t1r_test //tools/package/test:diag_t1t_test \
  //tools/package/test:diag_t1t_measured_test //tools/package/test:diag_t1v3_test //tools/package/test:diag_t1v3_measured_test \
  //tools/package/test:diag_t2_test //tools/package/test:diag_t2_audit_test //tools/package/test:diag_t2_run_docs_test \
  //tools/package/test:diag_t2_measured_test //tools/package/test:diag_d1min_audit_test //tools/package/test:diag_d1min_measured_test \
  //tools/package/test:diag_mmio_audit_test //tools/package/test:diag_blockers_docs_test //tools/package/test:diag_first_xnu_docs_test \
  //boot/loader/test:carveout_gate_test //boot/diag/loader/test:all //diagnostics/switch/test:all
bazel build --config=ios17 //boot:package //boot:erista_package //boot:diag_state //boot:diag_d1min //boot:diag_loader_r //boot:diag_t2
bazel build --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true //boot:erista_first_xnu
bazel build --config=ios17 --//install:prebuilt=local //boot:erista_first_xnu                     # must fail: no early_trace
bazel build --config=ios17 --config=release --//install:prebuilt=local //boot:erista_first_xnu    # must fail: release
```

Analysis only, nothing is built or run. The flag is off by default, so the first command must show the Erista row at boot 0 (no `BoardHeader` action), and the second the derived header and a `BoardAgree` action:

```
bazel aquery --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true 'mnemonic("BoardHeader|BoardAgree", //boot:erista_first_xnu)'
bazel aquery --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true --//boot:erista_boot_run=true 'mnemonic("BoardHeader|BoardAgree", //boot:erista_first_xnu)'
bazel aquery --config=ios17 --config=release --//install:prebuilt=local --//boot:erista_boot_run=true //boot:erista_first_xnu   # must fail: release
bazel aquery --config=ios17 --//boot:erista_boot_run=true //boot:erista_package                                               # no BoardHeader: the attribute is not set
```

Do not build with `--//boot:erista_boot_run=true`: that makes a bootable Erista package, which is not approved.

The two `must fail` builds are the check; their error text is worth sending. Report the action counts, the kernel window size (17543168 bytes in 87
blocks, entry `0xb53cc4c8` at the last report), the margin, and every hash. Building the package is not running it.

## 8. Mac results for `c8f428d0b` (reported 2026-11-26 23:41 JST)

Six tests passed: `board_header_test`, `boot_gate_rule_test`, `boot_script_board_test`, `erista_package_test`, `diag_first_xnu_docs_test`, `diag_t2_measured_test`.
Four `bazel aquery` runs, analysis only, all as expected:

| `--//boot:erista_boot_run` | Target | Result |
| --- | --- | --- |
| false | `//boot:erista_first_xnu` | no `BoardHeader`, no `BoardAgree` |
| true | `//boot:erista_first_xnu` | `BoardHeader` with `--early-trace --stop-after-vm --single-core --profile debug`; `BoardAgree` with the derived `SwitchBoard.h` and `boot-script.txt` as inputs and `boards.ok` as output |
| true, `--config=release` | `//boot:erista_first_xnu` | refused at analysis, for the intended reason |
| true | `//boot:erista_package` | no `BoardHeader`, no `BoardAgree` |

No boot=1 package was built. The reported text of the refusal is not in the repository; the rule's own messages are in `boot/defs.bzl` (`_derive_boards`).

### Action graph check

The aquery results above show the actions exist. This checks what they read. Analysis only, nothing is built. Run with the flag on, once with the trace kernel option and once without it:

```
F='--config=ios17 --//install:prebuilt=local --//kernel:early_trace=true --//boot:erista_boot_run=true'
bazel aquery $F --output=text 'mnemonic("BoardHeader|BoardAgree|BootScript|LoaderCompile|LoaderLink|BootBl33|BootPackage|EarlyTraceCheck", //boot:erista_first_xnu)' > aq.txt
for m in BoardHeader BoardAgree BootScript BootBl33 BootPackage EarlyTraceCheck; do echo "== $m"; awk -v RS= "/Mnemonic: $m/" aq.txt | grep -o 'kits/SwitchBoard/SwitchBoard.h\|[a-z_]*_work/boards/SwitchBoard.h\|[a-z_]*_work/boards.ok\|[a-z_]*_work/early-trace.ok' | sort | uniq -c; done
echo "== LoaderCompile: include order"; awk -v RS= '/Mnemonic: LoaderCompile/' aq.txt | grep -o -- '-I [^ ]*' | sort | uniq -c
```

Expected:

| Action | Real `kits/SwitchBoard/SwitchBoard.h` | Derived `..._work/boards/SwitchBoard.h` | `boards.ok` | `early-trace.ok` |
| --- | --- | --- | --- | --- |
| `BoardHeader` | input | output | none | none |
| `BoardAgree` | none | input | output | none |
| `BootScript` | none | input | none | none |
| `LoaderCompile` (every source) | none | in the inputs, and its directory is the `-I` | none | none |
| `BootBl33` | none | none | input | none |
| `BootPackage` | none | none | none | input |

A real-header count above 0 in `BootScript`, `LoaderCompile` or `BootBl33` is a finding: some reader would still see boot 0 (or the loader and script would disagree). `LoaderCompile` also must show no `-I` directory
that holds the real `SwitchBoard.h` ahead of the derived one (`loader.c` and `carveout.h` include it as `"SwitchBoard.h"`). Run the same listing without the flag: no derived header, no `BoardAgree`, the real header everywhere.

### Why `early_trace` off is refused, and where the files are

| Case | Where it stops | Message | Files |
| --- | --- | --- | --- |
| `require_early_trace` unset on a boot-row target | analysis | `... enables boot rows but has no require_early_trace` | `boot/defs.bzl` `_derive_boards` |
| release profile | analysis | `... needs the debug profile: the early marks exist only in DEVELOPMENT or DEBUG kernels` | `boot/defs.bzl` |
| `stop_after_vm` without the trace build | analysis | `... stop_after_vm needs require_early_trace` | `boot/defs.bzl` |
| the kernel was built with `--//kernel:early_trace=false` | build, action `EarlyTraceCheck` | `<kernel>: no SWITCH_EARLY_TRACE start.s found (early_trace=false or a release kernel)`, exit 1 | `tools/package/first_xnu_check.py`; kernel `bazel-bin/kernel/kernel.macho` (the file `//kernel:prelinked` builds, `ctx.file.kernel` of the package; there is no file named `prelinked`); stamp `bazel-bin/boot/erista_first_xnu_work/early-trace.ok` |
| a compressed or foreign kernel | build, same action | `start.s early-rendering enable not found` | same |

The binary the check reads is the one in the package: the kernel's `start.s` must hold the 16-byte sequence `ENABLE_SEQUENCE + TRACE_SEQUENCE` (`first_xnu_check.py`). The stamp `early-trace.ok` is an input of `BootPackage` only.

The gap this review found, `early-trace.ok` feeding `BootPackage` only, is closed in the next commit (below).

### `early-trace.ok` on the derived table and on bl33

`_boot_package_impl` now declares `EarlyTraceCheck` before the derived table. `BoardHeader` reads `early-trace.ok` and passes it to the tool as `--trace-stamp`; the tool refuses unless the file
holds exactly `early_trace`, which only `first_xnu_check.py kernel` writes and only for a kernel that has the trace. `BootBl33` takes the stamp directly when the target enables a row, `BoardAgree` stays an input of `BootBl33`,
and `BootPackage` still reads it. The script, the loader's compile and the loader's header inputs read the derived table, so they depend on the stamp through `BoardHeader`.
Whatever output is asked for (`boards/SwitchBoard.h`, `loader.uboot.bin`, `bl33.bin` or the package), a kernel without the trace stops the build at `EarlyTraceCheck`. The check graph now expects:

| Action | Reads `early-trace.ok` |
| --- | --- |
| `BoardHeader` | yes (input) |
| `BootBl33` | yes (input, only with a boot row) |
| `BootPackage` | yes (as before) |
| every other action, and every action of the normal and diagnostic packages | no |

Because the stamp is made before the derived table, a release or prebuilt-loader build with the flag on now stops at `... needs the debug profile: the early marks exist only in DEVELOPMENT or DEBUG kernels`
(`_boot_package_impl`). The Mac reported `... enables boot rows but has a release profile or prebuilt loader` for the old order; `_derive_boards` still carries that check as a second guard. Both are refusals.

Mac check for this commit, analysis only: repeat the four `aquery` runs of the table above; add `BoardHeader` to the `early-trace.ok` column of the loop (it must list the stamp), and confirm the normal packages' listings did not change.

### The XNU linker warning

The warnings (Mac, `early_trace=true`, build succeeded): `-new_linker is obsolete`; `-segment_order should list all segments, <segment> is missing` for `__PRELINK_TEXT`, `__PRELINK_INFO`, `__PLK_TEXT_EXEC`,
`__PRELINK_DATA`, `__PLK_DATA_CONST`, `__PLK_LLVM_COV`, `__PLK_LINKEDIT`, `__LINKINFO`; and `-segment_order of __LINKEDIT is ignored, the segment must be ordered last`. Read from the sources, nothing re-run:

| Question | Finding |
| --- | --- |
| Whose flags | Apple's. `kernel/xnu-10063.141.1/config/recipes_development.bzl` repeats `makedefs/MakeInc.def`: `-Wl,-new_linker` (recipe line 5596, MakeInc.def 663) and `-Wl,-segment_order,__TEXT:__DATA_CONST:__DATA_SPTM:__LINKEDIT:__TEXT_EXEC:...:__BOOTDATA` (recipe 5636, MakeInc.def 824). This repository did not add them |
| Why those eight segments | They are the ones the same link line creates with `-sectcreate` (`__PRELINK_TEXT`, `__PRELINK_INFO` at recipe 5593 and 5595, `__PLK_TEXT_EXEC`, `__PRELINK_DATA`, `__PLK_DATA_CONST`, `__PLK_LLVM_COV`, `__PLK_LINKEDIT` at 5630 to 5634, `__LINKINFO` at 5648). The order list does not name them. Eight of the eight match, so the list of warnings is explained by the flags and by nothing else |
| `__LINKEDIT` | The list names it fourth; the linker wants it last and ignores the entry |
| `-new_linker` | The flag selects Apple's newer linker; a current Xcode linker accepts and ignores it, which is the "obsolete" message. Which Xcode the Mac has is not recorded: `xcrun ld -v` |
| `early_trace` | No link. `kernel/BUILD.bazel` (lines 62 to 64) says only `start.s` reads `SWITCH_EARLY_TRACE`, so only its object changes; the link line is the same with the flag off. The plain build should print the same ten lines. Not yet seen |
| Erista | Nothing board specific: these flags are the same for the OLED build and for the Erista package, which differ in ADT, DTB and boot files, not in the kernel link |
| iOS 27 tree | `xnu-12377.121.6/config/recipes_*.bzl` has the same `-segment_order` (line 6611, 5548); whether it warns is not known |

What this could change, and what no evidence here settles: the linker places named segments in list order and the rest by its own rule, so a newer linker may order the unnamed segments differently from the linker Apple built with.
`__PRELINK_TEXT` has its own address (`-segaddr 0xfffffff004004000`), so it is not moved by that. Whether the built kernel's segment order matters to this repository's tools (prelink, window layout, `first_xnu_check.py`,
which matches 16 bytes of `start.s`) depends on the order the kernel has, which a build output shows and the sources do not. Nothing said here is a finding about the boot; the link flags are not changed.

The Mac ran the comparison on 2026-11-27: section 9.

## 9. Mac results for the kernel (reported 2026-11-27 00:02 JST, commit `56526e5e5`)

| Item | Result |
| --- | --- |
| Checked file | `bazel-bin/kernel/kernel.macho`, the output of `//kernel:prelinked`. Sections 2 and 8 said `bazel-bin/kernel/prelinked`; no such file exists, and they now say `kernel.macho` |
| `first_xnu_check.py kernel`, `early_trace=true` | exit 0 |
| `first_xnu_check.py kernel`, `early_trace=false` | exit 1 |
| Linker warnings, `early_trace` true against false | no difference (`diff` of the `ld: warning` lines) |
| Toolchain | Xcode 27.2, build 27B52, Apple ld-27106.1 |
| Prelink candidate | 59154432 bytes (as reported; which file it names was not said) |
| Not reported | the kernel window size and entry (17543168 bytes in 87 blocks, `0xb53cc4c8` at the earlier report); the old builds' warnings |

`LC_SEGMENT_64` order from `otool -l`: `__TEXT`, `__DATA_CONST`, `__TEXT_EXEC`, `__KLD`, `__LASTDATA_CONST`, `__LAST`, `__KLDDATA`, `__DATA`, `__BOOTDATA`, `__PRELINK_TEXT`, `__PRELINK_INFO`,
`__PLK_TEXT_EXEC`, `__PRELINK_DATA`, `__PLK_DATA_CONST`, `__PLK_LLVM_COV`, `__PLK_LINKEDIT`, `__LINKINFO`, `__LINKEDIT`.

What that shows, against the link line (`recipes_development.bzl`):

- The nine segments that the `-segment_order` list names appear in the list's order. The list's other names (`__DATA_SPTM`, `__TEXT_BOOT_EXEC`, `__PPLTEXT`, `__PPLTRAMP`, `__PPLDATA_CONST`, `__PPLDATA`, `__HIBDATA`) are absent from this kernel, which is allowed: the list names what may exist.
- The eight segments the warnings name are all present, after `__BOOTDATA`, in the order of their `-sectcreate` options on the link line. So the linker put the unnamed segments after the named ones, in command-line order.
- `__LINKEDIT` is last, so the ignored entry for it did no harm.
- The warnings are therefore harmless in what they report: no segment is missing and none is misplaced. This is a reading of the order only. Addresses, sizes and permissions have not been reported yet (next list).

The link flags stay as they are.

### Mach-O measured by the Mac (reported 2026-11-27 00:11 JST, `early_trace=true`)

18 `LC_SEGMENT_64`; every `maxprot` and `initprot` is 1, 3 or 5; no segment is writable and executable; `__LINKEDIT` is last.

| Segment | vmaddr | vmsize | file | prot (max/init) |
| --- | --- | --- | --- | --- |
| `__TEXT` | `0xfffffff007004000` | `0x1dc000` | not reported | 1/1 |
| `__TEXT_EXEC` | `0xfffffff007400000` | `0xa78000` | not reported | 5/5 |
| `__PRELINK_TEXT` | `0xfffffff00427c000` | `0x6a4000` | offset 25337856, size 6963200 | 1/1 |
| `__LAST`, `__PLK_LLVM_COV`, `__PLK_LINKEDIT` | not reported | 0 | | not reported |

`__start` is `0xfffffff0075cc4c8`, inside `__TEXT_EXEC`. The header's own entry (`LC_UNIXTHREAD`) has not been reported.

Answers, from the sources and the arithmetic. Nothing was run on the Mac for them:

| Question | Answer |
| --- | --- |
| Why `__PRELINK_TEXT` is at `0xfffffff00427c000`, not `0xfffffff004004000` | My expectation in this section was wrong: `-segaddr __PRELINK_TEXT,0xfffffff004004000` is on the link line of the raw kernel (`kernel_unprelinked`), and `//kernel:prelinked` is the prelink output (`kernel_prelink_switch`, slide `-0x1000000` in `kernel/prelink.bzl`) that re-lays out the kext segments. The address the file has is the one the repository pins for this IPSW: `ipsw/pins.bzl` kind `21H461` has `kernel_virt_base` `0xfffffff00427c000`, `boot_args_contract_test` and `native_xnu_layout_test` use it, and it is built into the kernel (`-DSWITCH_KERNEL_VIRT_BASE`). The measurement equals the pin. How `0x4004000` becomes `0x427c000` step by step (original kernelcache base plus the slide) was not derived here |
| Why `__TEXT` equals `-image_base` and `__PRELINK_TEXT` does not | The ld output's `__TEXT` was not moved (`0xfffffff007004000` is the link line's `-image_base`); only the kext segments were placed by the prelink |
| Empty `__LAST`, `__PLK_LLVM_COV`, `__PLK_LINKEDIT` | Normal. The last two come from `-sectcreate ... /dev/null`; `__LAST` has a protection set by `-segprot` and no section. The layout tool (`native_xnu_layout.py`) already skips segments of size 0, so nothing places them. `first_xnu_check.py macho` accepts them and prints a note |
| Header entry against `__start` | The tool compares the `LC_UNIXTHREAD` pc with the `__start` symbol from `LC_SYMTAB` and fails on a difference. Run it (below); this is not yet measured |
| Virtual entry `0xfffffff0075cc4c8` against physical `0xb53cc4c8` | `SWITCH_KERNEL_PHYS_BASE` is `0xb207c000` (`kernel/switch/include/switch/boot_layout.h`) and is where the lowest segment goes: `0xb207c000 + (0xfffffff0075cc4c8 - 0xfffffff00427c000)` = `0xb207c000 + 0x33504c8` = `0xb53cc4c8`, the entry of the earlier package reports. The offset is the same modulo 2 MiB (`0x7c000`), as `kernel/BUILD.bazel` requires. This is arithmetic on the reported numbers; the tool recomputes it from the file |

Adding the new mode to the commands (`first_xnu_check.py macho`, built from `tools/package/macho_check.py`; it needs no Xcode):

```
python3 tools/package/first_xnu_check.py macho bazel-bin/kernel/kernel.macho \
  --base 0xfffffff00427c000 --phys-base 0xb207c000 --entry-phys 0xb53cc4c8
# the same on the plain build, and the differences between the two builds:
python3 tools/package/first_xnu_check.py macho kernel-trace.macho --compare kernel-plain.macho \
  --base 0xfffffff00427c000 --phys-base 0xb207c000 --entry-phys 0xb53cc4c8
```

(`kernel-trace.macho` and `kernel-plain.macho` are copies of `bazel-bin/kernel/kernel.macho` made after each of the two builds.) `--base` comes from the kind's `kernel_virt_base` in `ipsw/pins.bzl`; the tool has no literal for any kind.

What the mode refuses (exit 1): a segment writable and executable in `initprot` or `maxprot`, `initprot` outside `maxprot`, a segment's file range outside the file or overlapping another's, overlapping virtual ranges, `filesize` above `vmsize`,
a non-empty segment whose address or size is not a multiple of `0x4000`, no or two `__LINKEDIT`, `__LINKEDIT` not last in the load commands, two segments with one name, no or two `LC_UNIXTHREAD`, an entry that is not 4-byte aligned or not in an executable
segment's file-backed range, an entry that differs from `__start`, a kernel `native_xnu_layout.parse_kernel` refuses, a lowest segment other than `--base`, a physical base whose offset differs modulo 2 MiB, an entry that does not map to `--entry-phys`.
What it only prints as a note: empty segments, zerofill (`filesize` below `vmsize`), a file offset that is not a multiple of `0x4000`, anything above `__LINKEDIT` in memory or in the file, `__LINKEDIT` not ending at the end of the file, a kernel without `__start`.
`--compare` lists the segments and the entry that differ; a difference is never an error. `ERISTA_ADT.md` and `ERISTA_FIRST_XNU_BOOT.md` record windows of 17539072 and 17543168 bytes with the same entry for two packages; the cause of the 0x1000 was not attributed to the kernel (the packages' boot-args and trees differ too).
If the two kernels differ, the explanation comes first: `early_trace` changes `start.s`'s object only.

`__PRELINK_TEXT`'s file offset 25337856 is `0x182a000`: a multiple of `0x2000`, not of `0x4000`. That is why a file offset that is not 16 KiB aligned is a note and not an error in the tool; the reported kernel has one. Its file size 6963200 is `0x6a4000`, equal to its `vmsize`, so it has no zerofill.

## 10. Mac results for `2a13ebdc2` (reported 2026-11-27 00:27 JST)

Six of six regression tests passed: `macho_check_test`, `erista_seeded_build_test`, `first_xnu_check_test`, `diag_first_xnu_docs_test`, `board_header_test`, `boot_gate_rule_test`.

`first_xnu_check.py macho` on the real `kernel.macho`, with `--base 0xfffffff00427c000 --phys-base 0xb207c000 --entry-phys 0xb53cc4c8`:

| Run | Result |
| --- | --- |
| `early_trace=true` | exit 0 |
| `early_trace=false`, `--compare` | no difference in segments or entry; exit 0 |
| Header entry | `LC_UNIXTHREAD` pc `0xfffffff0075cc4c8`, equal to `__start`, inside `__TEXT_EXEC` |
| Segments | all 18 pass on address, size, file range and permissions |

Notes the tool printed, identical for true and false:

- segments exist after `__LINKEDIT`, in virtual address and in file range
- `__LINKEDIT` ends at `0x1829ce8`, the file at `0x386a000`
- 3 empty segments
- `__DATA` zerofill `0x1f4000`, `__LINKEDIT` zerofill `0x2318`
- 5 segments have file offsets that are not multiples of `0x4000`

The linker configuration was not changed. These are measurements of one kernel build; they are not a boot check, and the notes are why those cases are notes and not errors.

From the list of open checks in section 9, now measured: the entry against `__start`, the same result on the plain build, and the virtual to physical entry (`0xb53cc4c8`). Still open: the builds with action counts and hashes, the loader margin, and the seeded build below.

## 11. The seeded boot=0 package: what to run and report

The package is built with the real board table (Erista boot 0). Its loader stops at `0x306` and its script says `WRONG_BOARD`, so it cannot boot the Erista. This checks the seed handling, the cache flags, the hashes and the scan. Run from the repository root. Report the fingerprint, exit codes and hashes only.

```
python3 tools/package/erista_seed.py generate                 # fingerprint only; --force replaces an existing seed
python3 tools/package/erista_seed.py check                    # exit 0: set, private, not the pinned seed
stat -f %Lp local/erista-seed.hex                             # 600
git check-ignore -v local/erista-seed.hex                     # prints the .gitignore rule
git ls-files --error-unmatch local/erista-seed.hex            # must fail: not tracked
python3 tools/package/erista_seeded_build.py build            # no extra flags
```

The wrapper analyses its own command first (`bazel aquery` with `BootScript|BoardHeader|BoardAgree`) and refuses unless `BootScript` is listed and the other two are not. That query reads the `Mnemonic:` lines of `--output=text`, which the Mac has not confirmed. If the build says "could not analyse", send the stderr; the boot row is not on, the check failed closed. To see the same query by hand:

```
bazel aquery --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true --//boot:erista_boot_run=false \
  'mnemonic("BootScript|BoardHeader|BoardAgree", //boot:erista_first_xnu_seeded)' --output=text | grep Mnemonic
```

Then, after the build:

```
bazel aquery --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true --//boot:erista_boot_run=false \
  //boot:erista_first_xnu_seeded --output=text | grep -ci "no-cache"           # format unverified: report the count and 3 lines of context
python3 tools/package/erista_seeded_build.py build            # second run: the seeded actions must run again, not come from a cache
shasum -a 256 bazel-bin/boot/erista-first-xnu-seeded/switchroot/*/*
python3 tools/package/erista_seeded_build.py scan             # then once more with --also FILE for any saved output
python3 tools/package/first_xnu_check.py margin bazel-bin/boot/erista_first_xnu_seeded_work/loader.uboot.bin
python3 tools/package/first_xnu_check.py kernel bazel-bin/kernel/kernel.macho
python3 tools/package/first_xnu_check.py macho bazel-bin/kernel/kernel.macho \
  --base 0xfffffff00427c000 --phys-base 0xb207c000 --entry-phys 0xb53cc4c8
cat bazel-bin/boot/erista_first_xnu_seeded_work/early-trace.ok     # early_trace
python3 tools/package/erista_seeded_build.py clean            # after the hashes are recorded
python3 tools/package/erista_seeded_build.py clean --seed     # last; a rebuild needs a new seed
```

On the second build, "executed" action counts in bazel's summary for the seeded targets are the evidence; a cache hit there is a failure to report. The `bazel-bin/boot/erista_first_xnu_seeded_work/` path is `name + "_work"` from `boot/defs.bzl`; send the error if the file is not there.

## 12. Mac results for the seeded build, and the wrapper after them (2026-11-27 01:01 JST)

Measured on the Mac for the boot=0 seeded package (fingerprint `4b7779b2`, seed check PASS). Section 11 was written before these; where they differ, this section wins.

| Item | Result |
| --- | --- |
| First build | success, 652.589 s, 13046 actions (7503 internal, 5537 darwin-sandbox, 6 local); kernel window 17543168 bytes, 87 blocks, entry `0xb53cc4c8` |
| Second wrapper build | 1.057 s, 1 internal action, nothing re-executed. Expected: bazel found every output up to date. It says nothing about the caches, which only a run that executes the actions can show |
| aquery, boot row | `BootScript` present, no `BoardHeader` or `BoardAgree` |
| aquery, `no-cache: 1` and `no-remote: 1` | `LoaderCompile`, `LoaderLink`, `BootScript`, `BootUBoot`, `BootBl33`, `OverlayTree`, `InstallerSeedPin`, `BootPackage`. The full list is still to be reported; `InstallerSeedPin` pins the Installer bundle and does not hold the seed |
| Seed leak scan | `no leak found`, from the old scan: git's tracked files only. It is not a leak check of caches or logs |
| Loader margin | `flat_bytes` 122880, `image_end` `0xa001e000`, to the stamps slot `0xf00` (3840), to the SD reference page `0x1000` (4096) |
| `first_xnu_check.py kernel`, `macho`, `early-trace.ok` | exit 0, exit 0, `early_trace`; the 18 segments, permissions, entry and notes match section 10 |
| `output_base` mode | 755: any user of the Mac can read the seeded outputs |
| Disk cache | `.bazelrc:19` `common --disk_cache=~/Library/Caches/switch-ios-bazel/disk`; `:31-33` garbage-collection settings; `:126` `build:reproducible-check --disk_cache=`; `:139` `build:remote` sets a smaller GC size |

The ordinary cache stays. The seeded build ends its command line with `--disk_cache=`, which wins over line 19; no rc line is changed or needs to be.

What the wrapper does now (host tool only; the package is still boot=0 and nothing runs on a console):

- Extra bazel arguments after `--` must be on a short list (`--jobs=N`, `--keep_going`, `--color=`, `--curses=`, `--announce_rc` and a few more); anything naming a remote, disk cache, build-event upload or credentials is refused by name, and so is everything else not listed. A flag that takes a value must be written `--name=value`.
- The cache and build-event flags (now also `--bes_backend=` and `--bes_results_url=`) and `--//boot:erista_boot_run=false` come last, after the config and every extra flag.
- `build` refuses when the output base is not mode 700, is not yours, or has an ACL, and prints `chmod 700 "<output_base>"`. This changes nothing for ordinary builds: it is the same user's own directory. A separate output base would cost a full rebuild with no cache (about 11 minutes) and a second server, so it is not the default.
- `build` and `actions` follow the seed through the action graph with `bazel aquery inputs(...)`: every action that reads the seed file or an output of one that does must carry `no-cache` and `no-remote`, and `BootAdt`, `KernelLayout`, `KernelWindow`, `BootScript`, `BootBl33` and `BootPackage` must be among them. It refuses on a missing tag, on a missing required action, on a graph with no reader of the seed, and on a query it cannot read. It does not go by a path containing "seeded". Process-less actions (`Symlink`, `FileWrite`, ...) need no tag; an action it does not know does. The text format of `aquery` (`Inputs:`, `Outputs:`, `ExecutionInfo:`) is assumed from the Mac's earlier output and not yet confirmed for these fields.
- `clean` removes `erista-full-ram-seeded.adt` as well as the work tree and the package, and only those; `--seed` adds the seed file, and `--deep` adds `bazel clean` (all build outputs; the first build took 652 s; never implied).
- `scan` walks directories, never follows a link, reads files in chunks, fails on a path that does not exist and on a file it cannot read, and prints how many files and places it covered and what it cannot see. A hit is printed as a path, never as the seed. `--caches` adds the disk caches named in any rc file, bazel's logs, `action_cache`, `server` and `sandbox` under the output base, and counts the seeded outputs as expected places. `execroot` and `external` are left out by design and said so. The seeded ADT must hold the seed, or the scan fails: it proves the scanner can see the seed. Not detected: compressed or encrypted data, other encodings, derived data, remote caches, the SD card.

Run on the Mac, from the repository root, in this order:

```
python3 tools/package/erista_seeded_build.py actions                  # the action-graph check alone; prints mnemonic counts
chmod 700 "$(bazel info output_base)"                                  # only if build says it must; then stat -f %Lp "$(bazel info output_base)"
ls -led "$(bazel info output_base)"                                    # no numbered ACL lines
python3 tools/package/erista_seeded_build.py clean                     # the seeded outputs only; the ordinary caches and packages stay
python3 tools/package/erista_seeded_build.py build                     # this time the seeded actions execute: report the process counts and any "cache hit"
python3 tools/package/erista_seeded_build.py scan --caches 2> scan.err   # exit 0; send only the summary lines
```

Report: the counts from `actions`, whether `build` accepted the output base, the process line of the rebuild (a "disk cache hit" or "remote cache hit" there is a failure), and the scan summary (files, places, unreadable count, exit code). Not seed bytes, not `scan.err` unless it names a path.

## 13. Mac results for `caa6b4fa7` (reported 2026-11-27 01:17 JST), and what closes the seed work

| Check | Result |
| --- | --- |
| Action-graph check (`actions`) | exit 0; 12 actions hold the seed, all uncacheable: `BootAdt` 1, `BootBl33` 1, `BootPackage` 1, `BootScript` 1, `KernelLayout` 1, `KernelTool` 1, `KernelWindow` 1, `LoaderCompile` 4, `LoaderLink` 1 (reported 01:23 JST) |
| Rebuild after `clean` | 158 actions executed |
| SHA256 of the package files before and after the rebuild | no difference, exit 0. The same seed gives the same package, so a rebuild does not change what goes on a card |
| `output_base` | mode 700 |
| `scan --caches` | 64863 files, no leak: scanned 64863 files in 22449 places for leaks and 373 files in expected places; 279345 symbolic links not followed, 0 special files, 0 unreadable; the seed is in 7 of the 373 expected files; exit 0 (reported 01:23 JST). There is no control line of its own: see below |

Compressed or encrypted data, other encodings, remote caches and the SD card are outside what that scan can see.

What exit 0 means for the control, from `run_scan` in `erista_seeded_build.py`: the seeded ADT is checked only if it exists under `bazel-bin/boot` when the scan starts; then it must be among the expected files that hold the seed, or the scan raises and exits 1. If the file is absent the check is skipped without a line. So exit 0 means "the ADT held the seed, or there was no ADT"; it does not say which. Here the rebuild had just recreated the ADT (`clean` removes it, and the package needs it), so the control most likely ran and passed, and 7 of 373 expected files holding the seed is consistent with that. That is an inference from the order of your commands, not a printed fact. The tool should print the control's outcome; that change is proposed, not made. To see it without a code change, before `clean`:

```
python3 tools/package/erista_seeded_build.py scan --also bazel-bin/boot/erista-full-ram-seeded.adt   # must FAIL (exit 1) and name that path; no other path
```

A failure naming exactly that file is the positive control working. Do not use that command as a gate.

The 12 actions: `KernelTool` is the seeded ADT's generator (the mnemonic `kernel/rules/inputs.bzl` gives `tool_output`). The nine counts sum to 12. Missing from them are the `KernelCompress` actions: `boot/defs.bzl` creates one per shard (`_WINDOW_SHARDS` is 128), each reads a `KernelLayout` output directory and compresses it, and the shard that holds the ADT's region holds the seed. The code tags all of them (`_requirements(ctx)`, and `erista_seeded_build_test` counts that every action call has it), but the action-graph check did not report them. The likely cause is that they read a directory output (a tree artifact) that `aquery inputs(...)` does not list as an input file. That is a blind spot of the check, not evidence that the actions are cacheable. To settle it on the Mac:

```
bazel aquery --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true --//boot:erista_boot_run=false \
  'mnemonic("KernelCompress", deps(//boot:erista_first_xnu_seeded))' --output=text > /tmp/kc.txt
grep -c 'Mnemonic: KernelCompress' /tmp/kc.txt                       # expect 128
grep -c 'no-cache: 1' /tmp/kc.txt; grep -c 'no-remote: 1' /tmp/kc.txt     # expect 128 and 128
```

The proposed fix, not made: also require both tags on every action of a fixed list of mnemonics regardless of what the taint walk found, and add `KernelCompress` with its count of 128 to the required set.

What would close the seed work for the boot=0 stage:

| Condition | State |
| --- | --- |
| Seed generated, private, git-ignored, not tracked; fingerprint `4b7779b2` | done |
| Wrapper build with every cache and upload flag last, extra flags allow-listed | done |
| Every action that holds the seed is `no-cache` and `no-remote` (12 of 12) | done for the 12 found; the 128 `KernelCompress` actions were not among them (below), so this row is not closed |
| Rebuild executes the seeded actions and gives the same hashes | done (158 actions, no difference) |
| Output base mode 700 | done |
| Scan of git, disk caches, logs and state | done: 64863 files, 0 unreadable, exit 0. Whether the ADT control ran is read from the source below |
| `clean` and `clean --seed` run and checked | not run (below) |
| The package copied to a card and its hashes compared | not done; needs no boot permission, the package is boot=0 |
| The written decision to use the seed in the first run | open (row 6) |
| A seeded package that can boot the Erista | does not exist, and the wrapper refuses to build one. That needs the boot=1 build under test (row 8), which is not approved, and its own design for the seed |

So the seed handling is complete for what exists, and only for it. A boot-capable seeded package is not covered by any of the above.

Closing commands. Run `scan` before `clean --seed`: it needs the seed. Do not run `clean --seed` while a rebuild or a card copy is still planned.

```
python3 tools/package/erista_seeded_build.py scan --caches        # last scan; send the summary lines and the exit code
python3 tools/package/erista_seeded_build.py clean                # the three seeded outputs only
ls bazel-bin/boot/erista_first_xnu_seeded_work bazel-bin/boot/erista-first-xnu-seeded bazel-bin/boot/erista-full-ram-seeded.adt   # each: No such file
find "$(bazel info output_base)/execroot" -name 'erista-full-ram-seeded.adt' -o -name 'erista_first_xnu_seeded_work'                 # nothing
ls bazel-bin/boot | head                                          # the ordinary outputs are still there
ls -l local/erista-seed.hex                                       # still present after plain clean
python3 tools/package/erista_seeded_build.py clean --seed         # the seed file as well
ls local/erista-seed.hex                                          # No such file
python3 tools/package/erista_seed.py check                        # must now fail: no seed
git status --short                                                # no seed file, nothing new
```

`clean` does not touch the disk cache, the action cache or the logs. They hold digests, not the seed (`scan --caches` checked them); there is no `--deep` need unless that scan finds a hit.

The full regression run on the latest commit, from the repository root. `bpmpfw_scan_test` is the one known failure under the Linux stand-in; report it by name if it fails on the Mac.

```
git pull && git log --oneline -1
bazel test --config=ios17 //tools/package/test:all //boot/loader/test:carveout_gate_test //boot/diag/loader/test:all //diagnostics/switch/test:all
bazel build --config=ios17 //boot:package //boot:erista_package //boot:diag_state //boot:diag_d1min //boot:diag_loader_r //boot:diag_t2
python3 tools/package/diag_images.py                              # nine hashes; diag_t2 7f5a91a4..., diag_loader_r c101d9c3... must not change
bazel build --config=ios17 --//install:prebuilt=local --//kernel:early_trace=true //boot:erista_first_xnu
bazel build --config=ios17 --//install:prebuilt=local //boot:erista_first_xnu                     # must fail: no early_trace
bazel build --config=ios17 --config=release --//install:prebuilt=local //boot:erista_first_xnu    # must fail: release
python3 tools/package/first_xnu_check.py kernel bazel-bin/kernel/kernel.macho
python3 tools/package/first_xnu_check.py macho bazel-bin/kernel/kernel.macho --base 0xfffffff00427c000 --phys-base 0xb207c000 --entry-phys 0xb53cc4c8
```

Never build with `--//boot:erista_boot_run=true`. Report the commit, the pass count, every hash and the text of the two failing builds.

## 14. The seeded package on a card: what it collides with (offline read, nothing copied)

Copying is on hold. This is what the rule in `boot/defs.bzl` (`BootPackage`) and `boot/BUILD.bazel` put in every boot package, so the same layout applies to `erista_first_xnu`, `erista_first_xnu_seeded` and the normal packages:

| Path in the package | Same path in a normal iOS package? |
| --- | --- |
| `switchroot/ios/*` (kernel window staging, `bl33.bin`, the loader and the rest of the switchroot files) | yes |
| `bootloader/ini/ios.ini` (`[iOS]`, `id=iOS`, `boot_prefixes=/switchroot/ios/`) | yes, the same file content |
| `bootloader/res/icon_ios_hue.bmp` | yes |
| `overlay/`, `bootstrap/payload.tar`, `bootstrap/manifest.json`, `installer-bundle.json` | yes (the last when the Installer bundle is present) |

So the first-XNU package, seeded or not, overwrites a card's existing iOS entry: same `ios.ini`, same `switchroot/ios/`. Two of them cannot coexist, and the seeded one holds the seed in its window. The diagnostic packages use their own entry names (`ios-diag-*`), which this read did not check against their file names; list the card before anything is copied.

From the repository root, read-only, nothing is written to the card:

```
find bazel-bin/boot/erista-first-xnu-seeded -type f | sort | sed 's|^bazel-bin/boot/||'
shasum -a 256 bazel-bin/boot/erista-first-xnu-seeded/bootloader/ini/ios.ini bazel-bin/boot/erista-ios/bootloader/ini/ios.ini   # equal: the entries collide
find /Volumes/<card> -maxdepth 3 \( -path '*/switchroot/*' -o -path '*/bootloader/ini/*' \) | sort    # what is on the card now
ls -la /Volumes/<card>/bootloader/ini /Volumes/<card>/switchroot
```

Report whether the card holds `ios.ini` or `switchroot/ios` already, and which diag entries. The run sheet's rows 5 and 6 (`ERISTA_FIRST_XNU_RUN.md`: the card holds only this entry's files; the normal Erista package is not on it) are what the copy would have to satisfy, with hashes taken before and after a re-insert. Using this seed for a later boot-capable build is not decided.
