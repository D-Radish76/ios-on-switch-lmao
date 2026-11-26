# Erista: the random seed

Design and host checks (N7-E). Nothing here touches the Security Engine, and `boot` stays 0.

## Who consumes it, and when (xnu-10063.141.1)

| Order | Consumer | Where | Mark in a trace build |
| --- | --- | --- | --- |
| 1 | `early_random()` for the physmap slide, first call: `bootseed_init` reads `/chosen/random-seed` (the first 64 of 256 bytes) | `arm_vm_init.c:1884` | stage 18 |
| 2 | `early_random()` for the stack guard | `arm_init.c:682` | after stage 21 |
| 3 | The kernel PRNG (`cckprng`), initialised from the same seed and then fed by `entropy_provide` | `prng_random.c:275` | |
| 4 | `write_random()` from a driver, later | `TegraSE.cpp` (after the AES, SHA and CMAC known answers) | board thread |

`PE_get_random_seed` zeroes the property after the read. Consumers 1 and 2 run before IOKit exists, so no kernel driver can supply
them. Only consumer 3 and later users can be helped from the kernel.

## Where the seed comes from now

`boot/BUILD.bazel` pins one 256-byte seed (`_ADT_SEED`) and the Erista ADT uses it too. It is in the repository, so the stack guard and
the physmap slide of an Erista boot are the same on every console and every boot and are known to anyone who reads the tree. The loader
mixes SE2's DRBG into it per boot (`boot/loader/seed.c`), but only where the row has `SWITCH_FEATURE_SE2`, and the Erista has none.
`TegraSE` starts only behind the same feature, so the later `write_random` is off as well.

## Options

| | Per boot | Reaches consumer 1 and 2 | Needs | Open |
| --- | --- | --- | --- | --- |
| A. SE1's DRBG in the loader | yes | yes | `seed.c` takes the engine base (`0x70012000`) and AHB master bit (14) from a per-row side table, as `carveout.h` does; a row-mask bit would change `switch_boards[]` and with it every pinned diagnostic image | whether EL2 may run an RNG operation on the Erista (`SE_SECURITY`); the loader's linked margin, not reported; the code fails safe to the pinned seed |
| B. A seed file on the card, refreshed by the kernel | from the second boot | yes | U-Boot copies it to the window's seed offset (a constant the package already computes, `SEED_ADT_OFFSET`); a kernel writer after boot | the first boot has the pinned seed; anyone who reads the card has the next seed; no forward secrecy |
| C. A seed generated per install on the Mac | no: per install | yes | the Erista ADT takes its seed from an install-time value instead of `_ADT_SEED`; nothing random enters a Bazel action | removes the public seed and the same-for-every-console property, not the same-every-boot one. The ADT then differs per install |
| D. A kernel driver reseeds `cckprng` after boot | yes | no | an SE1-only seed step, not `tegra_se_start` (its known-answer tests and timing run use more of the engine) | helps consumer 3 and `/dev/random` users only |

Recommended order, if approved: C first (offline, no hardware), then A once the SE1 security registers have been read (a read-only
diagnostic in the D0-S style) and the margin is known, D after A or if A cannot work. B only as the fallback for A.

## Option C as built (N7 follow-up C)

`tools/package/erista_seed.py generate` writes 256 bytes from the Mac's secure RNG to `local/erista-seed.hex` (mode 0600,
git-ignored, never printed; only an 8-digit fingerprint is). `//boot:erista_first_xnu_seeded` builds the first-XNU package from
that file through `//boot:erista_adt_seeded`. The seed is not patched into a built package: the window is LZ4-compressed
per megabyte, its SHA-256 is the stage id and the loader embeds the block table, so a changed seed changes the loader.
Rebuilding recomputes every check value (stage id, block table, the script's CRCs) and keeps Bazel's actions
deterministic for a given file. Unset, short, all-zero, low-variety, world-readable or pinned seeds are refused by the tool
and by the ADT step, and without the file the target does not exist to build.

Caches (N7-I). Every action downstream of the seeded ADT holds the seed in its inputs or outputs: `BootAdt`, the window layout and its
LZ4 shards, `KernelWindow`, the loader compiles that include `layout.h`/`runs.h` and the link, the boot script, `BootBl33` and the package copy. `boot_package`'s
`private_seed` attribute (set on `//boot:erista_first_xnu_seeded` only) gives all six of the rule's action calls `no-cache` and `no-remote`, and
`erista_seeded_build_test` counts that none is missed. Untainted actions of that target become uncacheable too, which only costs time.

`tools/package/erista_seeded_build.py` is the one way to build it. `build` loads the seed (refused when unset, short, pinned, low-variety or readable by others),
checks that git ignores it and does not track it, then runs bazel with `--remote_cache= --remote_executor= --noremote_accept_cached
--noremote_upload_local_results --disk_cache=` and refuses flags that would write build details to a log or file (`-s`, `--subcommands`,
`--verbose_failures`, `--profile`, build-event and execution-log files). The seed is never an argument: bazel receives the path `//local:erista-seed.hex`.
`scan` looks for the seed (hex or raw) in git's tracked files and in files named with `--also`, such as a saved log. `clean` removes the seeded outputs and, with `--seed`, the seed.

What stays on the Mac after a build: the seed file; `bazel-bin/boot/erista-full-ram-seeded.adt` and the `erista_first_xnu_seeded_work` tree (the ADT, window
shards, loader, bl33.bin); `bazel-bin/boot/erista-first-xnu-seeded` (the package that goes on the SD card); the SD card itself. The output base's action cache keeps digests, not
contents. A remote cache or a disk cache set in `~/.bazelrc` is overridden by the flags above. `bazel shutdown` and `clean` do not remove the seed file.
Unverified on the Mac: the flags' spelling for Bazel 9.3.1, that `no-cache` is honoured for every one of these actions, and `scan` against real outputs.

What this does not give: a seed per boot. Every boot of one install has the same physmap slide and stack guard, and the
seed file sits in the loader window on the card. SE1 is untouched (not approved).

## Not decided

- Whether a fixed stack guard and slide on the first test boots matter. A trace boot is not a security boundary; a daily-use install is.
- Who may approve a new per-row field or a side table for the loader.
- Any use of ECID: it is per console, not per boot, and is a stopgap.

## Host checks

`diagnostics/switch/test/random_seed_source_test.py` pins the order above and the gates.
