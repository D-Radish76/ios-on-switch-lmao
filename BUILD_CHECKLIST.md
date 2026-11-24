# Build checklist for the Mac

Nothing on the `erista-port` branch has been built with Bazel. This file is the list to run on an Apple silicon Mac, in
order, and what each step should show. The commands are the ones the repository documents (`README.md`, `tools/emu/README.md`,
`.github/workflows/ci.yml`) and the targets the changes added. Targets and flags marked "unchecked" were not run anywhere.

None of these steps boots the Erista. The board table keeps its row at `boot 0`, so a build never produces a card the
Erista accepts.

## What ran here, and with what

Linux has no Bazel execution platform, no GoogleTest and no `lz4`. The results below are the only ones that exist.

| Check | Result | Run with |
|---|---|---|
| `kits/SwitchBoard/test` (21 tests) | pass | GoogleTest stand-in, not GoogleTest |
| `boot/loader/test` board_args_test (3 tests) | pass | GoogleTest stand-in, not GoogleTest |
| `tools/package/test` boot_script_board_test (11), native_oled_adt_board_test (4), native_xnu_layout_test (22) | pass | Python `unittest`, no Bazel runfiles |
| `tools/mc_carveout` test_dump (7) | pass | Python `unittest`, fake `busybox` |
| `bazel/pinned/SHA256SUMS` | all files match, including `erista-sku0.dtb` | `sha256sum -c` |
| `SwitchBoard.h`, `SwitchBoardKernel.h`, the new `cpu.c` check | no warnings | clang `-Wall -Wextra -Werror`, shim headers, no XNU |
| `SwitchRootBoot.c` | compiles apart from two shim gaps | shim headers |
| `bl33_stage_test.py`, every other Bazel test, every build | not run | |

The stand-in is a small header that mimics `TEST`, `EXPECT_*` and `ASSERT_*`. A pass with it says the logic is right. It
does not say the test builds with the real GoogleTest, so the Mac run below is still needed.

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
   bazel test //kits/SwitchBoard/test:all //boot/loader/test:all //tools/package/test:all //tools/mc_carveout:all
   ```

   Expect `switch_board_test` (21), `board_args_test` (3), `boot_script_board_test` (11), `native_oled_adt_board_test` (4) and `test_dump` (7) to pass, and
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
