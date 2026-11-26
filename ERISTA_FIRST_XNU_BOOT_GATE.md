# First XNU: can `boot` 1 be limited to the diagnostic package?

Design of 2026-11-26 and its offline implementation. `SwitchBoard.h` is unchanged: `boot` stays 0 and the feature mask stays 0 for the Erista, and every hardware HOLD
stays. A build that boots the Erista is the owner's decision (`ERISTA_FIRST_XNU_BLOCKERS.md`, step 8) and is not approved.

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
| B. A board table for one package | `erista_first_xnu` (and `_seeded`) is built from a derived copy of the table in which only the Erista row's `boot` is 1; every other target keeps the real header | chosen by the owner; implemented offline, below |
| C. A runtime token (boot-args word or card file) the loader reads | the loader would trust something the card supplies, and a bad card state enables the boot | refused: a build-time guard fails closed, a runtime one does not |
| D. Edit the row locally for the run and revert | works, and one forgotten revert makes every Erista build bootable | refused: the guard rests on a person remembering |

## B, as implemented offline (2026-11-26)

The owner chose B. The mechanism exists; no package has been built with it, and building one is not approved.

| Piece | What it does |
| --- | --- |
| `tools/package/board_header.py derive` | writes a copy of `SwitchBoard.h` whose only difference is the named row's `boot` field, 0 to 1, then verifies that byte for byte. It refuses unless the command line states every condition (`--early-trace --stop-after-vm --single-core --profile debug`), the board is `erista`, the row still reads `t210b01 0, sku 0, boot 0, features 0`, and it is the only such row |
| `board_header.py verify`, `agree` | `verify` re-checks a derived file against the original; `agree` checks that the boot script accepts exactly the rows the derived table boots |
| `enable_boot_rows` on `boot_package` | empty by default. Only `erista_first_xnu` and `erista_first_xnu_seeded` (allowlist in `boot/defs.bzl`) may set it, to `["erista"]`, and analysis fails on release or a prebuilt loader, without `require_early_trace` or `stop_after_vm`, with more than one core, or if the table is not `kits/SwitchBoard:SwitchBoard.h` |
| `--//boot:erista_boot_run` (`boot/BUILD.bazel`) | the explicit build-time permission, default `False`. Only when it is `true` do the two targets pass `["erista"]`; otherwise they build from the real header and the Erista stays at boot 0 |
| One table for three users | the derived file replaces the real one in the `boards` attribute (the script action), in `loader_include_markers` (the loader's include directories) and in `loader_hdrs` (its header inputs), so the script gate, `loader.c`'s check and `carveout.h` read the same file. A `BoardAgree` stamp is an input of the bl33 stage |

`SwitchBoard.h` is unchanged: the Erista row stays `0, 0, 0, 0`. `erista_package`, the OLED package, `erista_rows_work` and every diagnostic image do not name the attribute, so their actions and hashes are as before. The kernel is not affected: it ignores `boot`.

Tests (host only, no Bazel run here): `board_header_test` (derive changes one character; every refusal: release, no early trace, no stop after VM, more than one core, unknown board,
a feature on the row, a header already at boot 1, a changed identity, a missing or doubled row, no table, a derived file with another change; a script from the other table; a compiled probe
that includes `carveout.h` shows the loader's table following the header it is given) and `boot_gate_rule_test` (allowlist, conditions, one table for all three users, flag default False, other packages without the attribute).

Not covered by any host test: Starlark itself, which only the Mac's analysis runs. `ERISTA_FIRST_XNU_MAC.md` section 7 has the analysis-only commands (`bazel aquery`, which builds nothing).

What B does not do: it does not approve a run, change what the first XNU package runs (single core, features 0, trace kernel, VM stop) or add protection against a hang. Whoever holds the flagged package holds a bootable Erista package,
so the card stays labelled and the run stays attended. The OLED row and the iOS 27 tree are untouched.

## What is still the owner's

Passing `--//boot:erista_boot_run=true` to a build that is meant for a card, and the written decision of the run. Neither is given.
