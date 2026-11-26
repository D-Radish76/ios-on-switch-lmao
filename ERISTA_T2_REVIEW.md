# T2: review before a run

T2 is not approved to run. This is an offline review of 2026-11-26 after D1-min. `boot` stays 0, the feature mask stays 0, T2, T3, T4, the full D1 and every XNU run stay on HOLD.
It reads the design (`ERISTA_T2_DESIGN.md`) against the code in `boot/diag/loader/t2_*.c` and `boot/diag/t2/t2.c` and the T1 v3 run.

## What T2 would do on hardware

The T1 v3 prelude (CAR checks, controller checks, three idle samples, divisor `00FF0001` once), then TMP451 transfers at 0x4C: ID, configuration, offsets, status and the old
sample as reads, and one write, the one-shot at pointer `0F` with value `00`. Then reads until BUSY clears or 100 ms pass, then configuration and the new sample.

| Item | Review |
| --- | --- |
| Writes new to real hardware | one: the TMP451 one-shot. Everything else is the T1 v3 transfer kinds, which ran (11 writes, 2 transfers, ID `55`) |
| The write is limited to the one-shot | `bus_write` stops with `T2_BUS_DENIED` for any pointer but `0F` or any value but `00`; `CNFG` `2802` and `2A02`, the two-byte forms, are used by that function alone |
| No register outside I2C1 and four CAR reads | `t2_regs.c` lists 13 addresses; the CAR four are read only |
| Entry state not assumed | the configuration is read and anything but `0xC0` stops before the write |
| Stop and failure | each failure stops with a `07xx` code, no retry, nothing restored; the sensor's shutdown bit is shown afterwards |
| The image | `diag_t2` is its own image with its own entry (`ios-diag-t2`) and the seven earlier hashes unchanged; flat hash `7f5a91a4...` on Linux |
| The script | `diag_t2` uses the generic loader script (`diag/loader/u-boot-script.txt`), which makes no `mmc` call and reads no device register |

## Findings

1. **No run procedure existed, and now one does** (`ERISTA_T2_RUN.md`, 2026-11-26). T1 v3 has `ERISTA_T1V3_RUN.md` (entry, screens, deadlines, stop rules). T2 has none: the design's "Stop codes `07xx`, the 15 s countdown and the 50 s deadline as T1 v3"
   could not be followed by a person without one. It is written; the Mac's package hashes are still to be filled in.
2. **A document error, corrected.** `ERISTA_T2_DESIGN.md` and `ERISTA_T1V3_RUN.md` gave `CONFIG_LOAD` as `+94`. The code (`t1v3_regs.c`, `t2_regs.c`) uses `+8c`, which is where T1 v3 read `00000004`
   and where `TIMING_0` (`+94`) is a different register. Both documents now say `+8c`. No code changes.
3. **The one-shot's form is a documented inference.** The data sheet says any value written to pointer `0F` starts a conversion; hekate and Linux give no precedent for the write-byte form on this part. If the sensor does not
   start, T2 shows the 100 ms timeout; nothing is lost, and the part stays in shutdown.
4. **The sensor's state afterwards.** T2 writes no setting, so shutdown stays; the next Nyx start runs `tmp451_init`. This is a design claim. T1 v3 sent only a pointer byte (`FE`) to the sensor, which sets no register, so no run shows what a write does to the part.
5. **Real-hardware I2C writes are not approved.** The one-shot is the first transfer that carries a value to a sensor register (T1 v3's pointer byte set no register). It needs its own approval, separate from this review.
6. **The audit tool had no `--t2` mode, and now has one** (`diag_mmio_audit.py --t2 --t2-sources`, with `diag_t2_allowlist.py`). Its Linux output is `boot/diag/erista-t2-mmio-audit.txt`; `diag_t2_audit_test` holds the negative cases. The Mac repeats it on its own build.
7. **What T2 does not buy.** It shows one temperature after a one-shot and the BUSY semantics. It does not give a thermal limit, does not run the fan, and does not make XNU's early heat known.

## Prerequisites before a run could be approved

- Done offline: `ERISTA_T2_RUN.md`, `diag_mmio_audit --t2` and its record, `diag_t2_audit_test`.
- Still open: the Mac builds `//boot:diag_t2`, repeats the audit and reports the package hashes (the sheet's section 2 waits for them); the owner's separate written approval for the one sensor write.

T2 can also be waived (`ERISTA_FIRST_XNU_BLOCKERS.md`): the first XNU diagnostic needs only a Nyx reading before and after, not an in-diagnostic temperature.
