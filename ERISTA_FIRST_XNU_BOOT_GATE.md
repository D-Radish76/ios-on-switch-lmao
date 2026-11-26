# First XNU: can `boot` 1 be limited to the diagnostic package?

Offline design of 2026-11-26. Nothing here changes a row or a build rule. `boot` stays 0 and the feature mask stays 0 for the Erista, and every hardware HOLD
stays. Setting `boot` 1 for any build is the owner's decision (`ERISTA_FIRST_XNU_BLOCKERS.md`, step 8).

## Where `boot` is read

| Reader | What it does with a 0 |
| --- | --- |
| `tools/package/boot_script.py` (`board_gate`), from the `boards` attribute of `boot_entry` | the U-Boot script keeps `handoff_status` at `WRONG_BOARD` for that row |
| `boot/loader/loader.c`, `if (!board->boot) refuse(0x306)` | the loader refuses the console |
| `boot/loader/carveout.h` | a row with `boot` 0 gets no MC carveout values (`NULL`), so the loader's table holds the Erista's values only when its row boots |

The kernel does not read the flag: `SwitchBoardKernel.h` keys its feature words by row and ignores `boot`. The diagnostic images print the real table's `BOOT=0`
and are built from it.

## Options

| Option | Effect | Verdict |
| --- | --- | --- |
| A. Flip the Erista row in `SwitchBoard.h` | every Erista package passes both checks, the normal `//boot:erista_package` included, with no stop after VM | refused: it removes the guard from packages that were never meant to start |
| B. A board table for one package | `erista_first_xnu` (and `_seeded`) is built from a derived copy of the table in which only the Erista row's `boot` is 1; every other target keeps the real header | recommended, below |
| C. A runtime token (boot-args word or card file) the loader reads | the loader would trust something the card supplies, and a bad card state enables the boot | refused: a build-time guard fails closed, a runtime one does not |
| D. Edit the row locally for the run and revert | works, and one forgotten revert makes every Erista build bootable | refused: the guard rests on a person remembering |

## B in detail

`boot_entry` already takes the table per target, with defaults from the macro in `boot/package.bzl`: `boards` for the script, `loader_include_markers` for the directory the
loader's `#include "SwitchBoard.h"` resolves in, and `loader_hdrs` as an input. Pointing all three at one derived header changes the script gate, the loader check and
the carveout values together, and nothing else. The Erista's values already exist in `carveout.h` (`switch_carveout_ERISTA`, read on this console by D0).

What it would take, none of it done:

1. A build action runs a small tool (`tools/package/board_header.py`) over the real header. It refuses unless the `erista` row has `boot` 0 and `features` 0, then writes the header with that one
   field set to 1. It never touches the features, the OLED row, or any other field, and it fails on a table it cannot read.
2. A new `boot_entry` attribute (for example `enable_boot_rows = ["erista"]`) selects the derived header. The rule calls `fail()` unless `stop_after_vm` and
   `require_early_trace` are both set, the way `stop_after_vm` already needs `require_early_trace`. A normal package cannot carry it.
3. Tests, all host-side: the derived header differs from the real one in that one field; the tool refuses an Erista row with a feature bit or with `boot` already 1; the normal
   Erista package and the OLED package aquery to the real header; `carveout_gate_test` keeps the Erista refused under the real header and accepts it under the derived one;
   `boot_script_board_test` shows one extra `READY` clause for the derived table only; `diag_state_test` and the diagnostic hashes stay the same.
4. The Mac builds `//boot:erista_first_xnu` and the normal Erista package, and the hashes of the normal package and the diagnostics do not move.

What B does not do: it does not approve a run. It does not change what the first XNU package runs (single core, features 0, trace kernel, VM stop), and it
adds no protection against a hang. Whoever copies the first-XNU package to a card still holds a bootable Erista package, so the card is the person's to keep
labelled and the run stays attended. The OLED row and the iOS 27 tree are untouched: the table is shared, and B changes no row in it.

## Decision for the owner

Choose B, or accept A or D in writing for one run knowing what they expose, or leave step 8 open. With B chosen, the implementation is a separate approved
change to build rules and host tests; no device code, MMIO allowlist or diagnostic image changes.
