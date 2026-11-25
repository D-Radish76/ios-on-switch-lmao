# Build checklist for the Mac

The Bazel test run of commit c790f50e is recorded below. Nothing else on the `erista-port` branch has been built with
Bazel yet. This file is the list to run on an Apple silicon Mac, in
order, and what each step should show. The commands are the ones the repository documents (`README.md`, `tools/emu/README.md`,
`.github/workflows/ci.yml`) and the targets the changes added. Targets and flags marked "unchecked" were not run anywhere.

None of these steps boots the Erista. The board table keeps its row at `boot 0`, so a build never produces a card the
Erista accepts.

## Run on the Mac

Commit `c790f50e4`, Apple silicon, Bazel 9.3.1, Xcode 27.2 (27B52), with the real GoogleTest:

```sh
bazel test //kits/SwitchBoard/test:all //boot/loader/test:all //tools/package/test:all
```

Result as reported: `Executed 18 out of 18 tests: 18 tests pass.` in 30.314 s. Every target in those three packages
passed, so the Bazel entries for `switch_board_test`, `board_args_test`, `boot_script_board_test`,
`native_oled_adt_board_test` and `diag_boot_test` build and run there.

Commit `eb0758e6`, same Mac: `bazel build //:boot --config=ios17 --jobs=4 --verbose_failures` finished with
`Build completed successfully, 151 total actions`. It first failed at `//bootstrap:payload`: codesign refused a Sileo.app
whose main executable or Info.plist was a symlink out of the Bazel tree (`a9b1753c` made the message readable,
`eb0758e6` copies such links as files in the signing copy). The build has no `boot` option on: the Erista row stays at
`boot 0`.

Commit `fb0b61ef6`, same Mac: `bazel test //tools/package/test:all --jobs=4 --test_output=errors` gave
`Executed 2 out of 7 tests: 7 tests pass.` (`diag_boot_test` and `bl33_stage_test` re-ran, the other five were cached
passes). The diagnostic package built there: `bl31.bin` 45 KB, `bl33.bin` 455 KB, `script.txt` 6.9 KB,
`ios-diag.ini` 155 B. On an Erista it ran once, see the next paragraph.

First diagnostic run on an Erista, commit `c6931169` (package built on the Mac, card in an experiment 32 GB FAT32 SD, hekate 6.5.5,
Nyx `More configs`): `t210b01=0 sku=0`, `status=READY console=OK`, `DONE: all 27 registers read`, a 15 second wait, `reset`, and
hekate's main menu came back by itself. About 21 seconds on video, no freeze, no loop. `diag_boot.py compare` on the Erista's values: 25 same, 0 DIFFERENT, 0 NOT READ, kept as `boot/diag/erista-first-run.log` (rows written
from the table, see its header) and checked by `diag_boot_test`; a `switch_board_test` case takes the measured HIDREV and ODM4 to the `erista` row
(run here with the stand-in gtest only: run `bazel test //kits/SwitchBoard/test:all //tools/package/test:all`). Nothing else was started: `boot` stays 0 and iPadOS did not boot.

Not run yet: the ios27 and release builds, the step 1 style checks, `//tools/mc_carveout:all`, `bazel test //...`, and
the emulator checks. The per target counts were not part of the report.

## What ran here, and with what

Linux has no Bazel execution platform, no GoogleTest and no `lz4`. The results below are the only ones that exist.

| Check | Result | Run with |
|---|---|---|
| `kits/SwitchBoard/test` (21 tests) | pass | GoogleTest stand-in here; real GoogleTest on the Mac (above) |
| `boot/loader/test` board_args_test (3 tests) | pass | GoogleTest stand-in here; real GoogleTest on the Mac (above) |
| `tools/package/test` boot_script_board_test (11), native_oled_adt_board_test (4), native_xnu_layout_test (22) | pass | Python `unittest`, no Bazel runfiles |
| `tools/mc_carveout` test_dump (7) | pass | Python `unittest`, fake `busybox` |
| `bazel/pinned/SHA256SUMS` | all files match, including `erista-sku0.dtb` | `sha256sum -c` |
| `SwitchBoard.h`, `SwitchBoardKernel.h`, the new `cpu.c` check | no warnings | clang `-Wall -Wextra -Werror`, shim headers, no XNU |
| `SwitchRootBoot.c` | compiles apart from two shim gaps | shim headers |
| `bl33_stage_test.py`, every other Bazel test, every build | not run | |

The stand-in is a small header that mimics `TEST`, `EXPECT_*` and `ASSERT_*`. A pass with it says the logic is right. It
does not say the test builds with the real GoogleTest, so the Mac run below is still needed.

## Fetching what Bazel times out on

On the first `bazel build //:boot --config=ios17` two downloads failed on the Mac: the LLVM 23.1.4 archive (`Read timed
out`, which Bazel 9.3.1 does not retry, bazelbuild/bazel#32132) and the IPSW's `os` member (`Multiple entries with same key`:
the retry after a dropped connection adds a second `Range` header to ours). Fetch both with a resumable `curl`, then
store them in the repository cache under their pinned sha256. Bazel finds them there and does not download. The
checks stay: nothing is stored unless its sha256 matches. Run from the repository root.

```sh
# IPSW 21H461, about 6.8 GB. All eight pinned members are cut out of it, 8 MiB at a time.
curl -L -C - --retry 30 --retry-delay 5 --retry-all-errors -o ~/Downloads/21H461.ipsw \
  "$(python3 -c "ns={}; exec(open('ipsw/pins.bzl').read(), ns); print(ns['IPSWS']['21H461']['url'])")"
python3 tools/seed_cache/seed_cache.py ipsw --pins ipsw/pins.bzl --kind 21H461 --ipsw ~/Downloads/21H461.ipsw

# LLVM 23.1.4. The url and sha256 are LLVM_RELEASE in bazel/llvm/llvm.bzl.
curl -L -C - --retry 30 --retry-delay 5 --retry-all-errors -o ~/Downloads/llvm.tar.xz \
  https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-macOS-ARM64.tar.xz
python3 tools/seed_cache/seed_cache.py file \
  --sha256 5d3179db39adeca5b17a34d82708fe976bc60e055b05fe557878fb3fb64d39ef ~/Downloads/llvm.tar.xz
```

Both print `stored` per entry and exit 1 on a `MISMATCH`. Do the same for `24B119` before an ios27 build. The cache is
`--repository_cache` in `.bazelrc`; pass `--cache DIR` to `seed_cache.py` if you changed it.

Done on the Mac for 21H461 and LLVM: sha256 matched, entries stored, the build then ran past both. The tool in this
repository is the same procedure with its own test (`//tools/seed_cache:test_seed_cache`); the version run on the Mac was
a hand written one with the same 8 MiB steps, so run this one once and check it prints `stored` eight times.

## Before you start

- Bazelisk, not a plain Bazel: `.bazelversion` pins 9.3.1.
- Xcode 27.2 build 27B52 exactly (`bazel/host/xcode.bzl`). Any other build is refused. `bazel run @xcode//:print_pins`
  shows what the repo expects, and `xcode-select -p` what you have.
- The Metal toolchain installed for that Xcode, about 35 GB of free disk, and the IPSW download on the first build.
- Run from a clean checkout of `erista-port`. `.bazelrc` has the `ios17`, `ios27`, `debug` and `release` configs.

Report back: the first failing command with its full output, `bazel version`, `xcodebuild -version`, and for any
failed test its `test.log` under `bazel-testlogs`. A pass needs no log.

## On the Mac

1. Dependencies and style (as CI does):

   ```sh
   bazel mod deps --lockfile_mode=error
   bazel test //:format_test //:lint_test //:clang_tidy_test
   bazel run //tools/lint:py_owners
   ```

   Expect the new files (`BUILD_CHECKLIST.md`, `board.h`, the test files) to need formatting or lint fixes. Report them.

2. The tests this work added, with the real GoogleTest:

   ```sh
   bazel test //kits/SwitchBoard/test:all //boot/loader/test:all //tools/package/test:all //tools/mc_carveout:all //tools/seed_cache:all
   ```

   Expect `switch_board_test` (21), `board_args_test` (3), `boot_script_board_test` (11), `native_oled_adt_board_test` (4), `diag_boot_test` (12), `test_seed_cache` (4) and `test_dump` (7) to pass, and
   `bl33_stage_test` to run for the first time (needs `lz4` on the Mac). If `boot_script_board_test` cannot find
   `native_oled_adt`, the new dependency in `tools/package/test/BUILD.bazel` is wrong.

3. Both iOS versions, debug and release. The shared tree changed, so both XNU trees compile it:

   ```sh
   bazel build //:boot --config=ios17
   bazel build //:boot --config=ios27
   bazel build //:boot --config=ios17 --config=release
   bazel build //:boot --config=ios27 --config=release
   ```

   (`--config=ios17` and `--config=ios27` are in `.bazelrc`; combining them with `--config=release` is unchecked.)
   What to watch for: `diagnostics/switch/SwitchRootBoot.c`, `drivers/SwitchPower/SwitchPMIC.cpp`,
   `drivers/NativeKeyStoreEvents/AKSDeviceKey.cpp` (new header dependencies `kits/SwitchBoard` and `kits/SwitchCAR`), and
   `osfmk/arm64/cpu.c` in both trees (`<switch/board.h>`, built with XNU's own warning flags). A `static const` table in a
   header can trip `-Wunused-const-variable` in XNU's flags. Release must show no new diagnostics.

   First Mac run of step 3 (commit `c790f50e4`) failed in three places, now changed: `SwitchPMIC.cpp` could not find
   `kits/SwitchBoard/SwitchBoardKernel.h` (`kits/SwitchBoard` was missing from its `hdr_deps` in
   `drivers/SwitchPower/BUILD.bazel`), and the bootstrap's `ellekit` and `Sileo` builds (`BootstrapSources`) were refused
   because the rule marks them no-sandbox and `.bazelrc` allowed only `darwin-sandbox`. Only that mnemonic now has
   `--strategy=BootstrapSources=remote,local`. Re-run step 3 and report the next error. The bootstrap builds now run
   outside the sandbox, so a failure there is no longer a sandbox message.

4. The whole suite:

   ```sh
   bazel test //...
   ```

5. The emulator checks, for both kinds. These run a kernel without the loader's `switch-board` word, so they depend on
   the emulated registers answering as an OLED (the fuse page does; HIDREV is unconfirmed):

   ```sh
   bazel run //tools/emu:check -- --kind=ios17 --until launchd
   bazel run //tools/emu:check -- --kind=ios27 --until launchd
   ```

   If the kernel starts no board drivers, look for `SwitchBoard` resolving to the unknown board: the emulated HIDREV or
   the device tree is the first thing to check. That is the intended failure, not a bug in the gate.

6. The pins:

   ```sh
   (cd bazel/pinned && sha256sum -c SHA256SUMS)
   ```

## Still not covered by any of this

The Erista's DTB, `native_gate` MC values, every feature bit's review, and every boot on a console. See
`ERISTA_ROADMAP.md`.

## D0 loader diagnostic (Mac, before any hardware run)

```sh
bazel test //tools/package/test:diag_loader_test //tools/package/test:diag_boot_test //boot/diag/loader/test:all
# diag_loader_test now also reads the DTB's reservations (tools/package/fdt_ranges.py); 46 tests passed on Linux, not on Bazel
bazel build //boot:diag_loader --config=ios17
```

`//boot:diag_loader` must pass its own inspection (the link fails otherwise). Report, from `bazel-bin/boot/diag_loader/`:

```sh
cat inspection.txt                  # first line: D0 loader inspection passed
grep -cE 'loader_enter_kernel|loader_blocks|exporter' loader.map     # 0
shasum -a 256 loader.uboot.bin package/switchroot/ios-diag-loader/*  # for the review
ls -l package/switchroot/ios-diag-loader package/bootloader/ini
```

The measured first-run log replaces the fixture (path of your `LOG.txt` in place of `~/LOG.txt`):

```sh
PYTHONPATH=tools/package python3 tools/package/diag_boot.py adopt ~/LOG.txt --loader-src boot/loader/loader.c \
  --source "Erista HAC-001, hekate 6.5.5, diagnostic commit c6931169, log saved from the run's screen" \
  --out boot/diag/erista-first-run.log
PYTHONPATH=tools/package python3 tools/package/test/diag_boot_test.py
```

`adopt` refuses a log without all 27 register lines in the script's order. Not run on hardware: the loader diagnostic waits for review.

## D0-pre memory diagnostic

```sh
bazel test //tools/package/test:diag_memory_test //tools/package/test:fdt_ranges_test //tools/package/test:diag_loader_test
python3 tools/package/diag_memory.py build --frame boot/diag/memory/u-boot-script.txt \
  --page1 boot/diag/memory/page-1-bdinfo.txt --page2 boot/diag/memory/page-2-fdt.txt \
  --boards kits/SwitchBoard/SwitchBoard.h --bl31 bazel/pinned/firmware/bl31.bin \
  --bl33 bazel/pinned/firmware/bl33.bin --out "$PWD/build/diag-memory"
(cd build/diag-memory && find . -type f | sort | xargs shasum -a 256)
```

`bdinfo` in `diag_memory_test` is skipped when no AArch64 `llvm-objdump` is on PATH; on the Mac, run it with Xcode's
(`xcrun --find llvm-objdump` on PATH) so it does not skip. Not run on hardware.

