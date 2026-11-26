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

## Not decided

- Whether a fixed stack guard and slide on the first test boots matter. A trace boot is not a security boundary; a daily-use install is.
- Who may approve a new per-row field or a side table for the loader.
- Any use of ECID: it is per console, not per boot, and is a stopgap.

## Host checks

`diagnostics/switch/test/random_seed_source_test.py` pins the order above and the gates.
