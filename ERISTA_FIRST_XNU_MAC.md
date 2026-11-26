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

Why that order: `scan` runs right after the build and again before anything is committed or posted, because it reads git's tracked files and the files named with `--also`, nothing else. `clean`
removes the package, so it comes after the copy; the seed file goes last because a rebuild with a new seed gives a different package and the card's copy holds this one. `scan` does not look inside `bazel-bin`
or the card, which hold the seed by design: the window on the card contains it, so the card is not shared and the entry's files are removed after the run.

The seeded target carries the `no-cache` and `no-remote` tags, and the seed file is git-ignored, so it is never committed.

Never: commit, upload, paste or log the seed; put the seeded package or its logs in a remote or disk cache; pass the seed on a command line (bazel gets only the path `//local:erista-seed.hex`). A report carries
the 8-digit fingerprint and file hashes, nothing derived from the seed's bytes. `//tools/package/test:erista_seed_test` and `erista_seeded_build_test` cover the tool and the cache tags.
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
| the kernel was built with `--//kernel:early_trace=false` | build, action `EarlyTraceCheck` | `<kernel>: no SWITCH_EARLY_TRACE start.s found (early_trace=false or a release kernel)`, exit 1 | `tools/package/first_xnu_check.py`; kernel `bazel-bin/kernel/prelinked` (the file `//kernel:prelinked` builds, `ctx.file.kernel` of the package); stamp `bazel-bin/boot/erista_first_xnu_work/early-trace.ok` |
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

To close it on the Mac (no change to any flag):

```
bazel build --config=ios17 --//kernel:early_trace=true //kernel:prelinked 2>&1 | tee kernel-link-trace.log | grep -i warning
bazel build --config=ios17 --//kernel:early_trace=false //kernel:prelinked 2>&1 | tee kernel-link-plain.log | grep -i warning
diff <(grep -i warning kernel-link-trace.log) <(grep -i warning kernel-link-plain.log)       # expected: no difference
xcrun ld -v 2>&1 | head -2; xcodebuild -version
for k in trace plain; do xcrun otool -l <that kernel> | awk '/segname/{s=$2} /vmaddr/{print s, $2}' > seg-$k.txt; done   # the kernel before prelink, from `//kernel:kernel`
```

Report the diff, the Xcode version and the two segment lists. In the segment list, check that `__LINKEDIT` is last, that the order of the named segments is the list's, and that the 17543168-byte, 87-block window and entry
`0xb53cc4c8` of the last report are unchanged. If the old builds printed the same warnings, the toolchain did not change them.
