# D1 loader diagnostic

D0-R plus the normal script's exporter and SD reference steps, behind more checks than the normal script makes. Hekate
entry `iOS Diag Loader SD`, files under `switchroot/ios-diag-loader-sd/`. D0, D0-R and D1-pre are built as before and
share no output with it. The Erista's row stays `boot 0`. No kernel, no staged window, no write to the card.

```sh
bazel build //boot:diag_loader_sd --config=ios17
```

Run steps and stop rules: `RUN_CHECKLIST.md`. The reasoning behind each check: `ERISTA_D1_DESIGN.md`.

## What the script does

1. D0-R's board, framebuffer and DTB steps.
2. Clears the status block at `0xA700F000` (inside the exporter's run area, above its image).
3. Checks the exporter in its slot: all 16 header words, then its data CRC32.
4. Checks the three instruction words of each U-Boot cache helper (`0xAA001920`, `0xAA001A64`).
5. `bootm start` with `verify yes` (this U-Boot checks the data CRC there, in the slot), then `bootm loados`.
6. CRC32 of the loaded image at `0xA7000000`, then `go` into each helper. They return 1 whatever they are given, so `go`
   reporting success means different code ran.
7. Loads the loader as in D0 (`verify no`), `bootm prep`.
8. Reads sector 0 of `mmc 1` into `0xA001F200`: one attempt, no retry, no other device.
9. CRC32 of the image once more, then `go 0xA7000000`. The record at `0xA001F400` is checked for magic, version and size.
10. Writes the reference header, `bootm go`. The loader shows the status block and runs `sd_reference.c`'s validation.

Any failed step names its status, waits 15 s and resets. None of them reaches a later `go` or `bootm go`.
`diag_sd_loader_test.py` injects each one.

## Why it is the way it is

- The status block carries what the script measured to the loader through RAM, so the screen shows the CRC values and
  which steps ran, not only that the loader started. Its magic is written last: a block without it is reported missing.
- The card is never shown by identity. The record holds CID and CSD, so the screen prints capacity, OCR, RCA and the CSD
  structure version, and says the rest is not shown. Fixtures hold synthetic values.
- Link inspection (`tools/package/diag_loader_inspect.py --sd`) allows the controller address `0x700B0000` only as a
  compare constant in the validator. A load, a store or a copy of it anywhere fails the link.
- `bootm loados` cleans the D-cache over what it loads (`dc civac`) and `go` does no cache work, so the one step that makes
  the exporter's bytes current for instruction fetch is the first helper (`ic ialluis; isb`); the second is a `dsb sy; isb`
  that is the tail of the TLB invalidate. This is read from the pinned binary, not observed; a hang at `go 0xA7000000` is
  the symptom of being wrong. Details: `ERISTA_CACHE.md`.
