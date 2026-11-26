# D0-S v2

D0-S (the first one) hung after its framebuffer line. The instruction that hung is not identified. `boot/diag/erista-d0s-run1.txt` has the run. D0-S stays as it is and is
not run again. v2 is a separate image and a separate script. Nothing is approved to run: `boot` 0, feature mask 0, every HOLD.

## What the run shows

The script printed the framebuffer line. The next lines were two U-Boot loads of CAR words, the SDMMC1 gate, `sleep 5` and the
handoff. The screen never moved on. Several places fit that, and none is shown to be the cause: the load of `0x60006004`, of `0x60006010`, of an SDMMC1 word, or a later step whose text the video console had
not drawn yet. The run cannot tell them apart, and there is no UART.

What is known:

- The pinned U-Boot (`2024.NX02.b201801`) has `setexpr` and its `*` load form. The OLED script uses `setexpr.l NAME *0x60005010` for its
  timer stamps. No record here says it ever ran on an Erista.
- T1-T read both CAR words from the EL1 loader on the same console and got `RST_DEVICES_L 049102CA`, `CLK_OUT_ENB_L 9802D131`
  (`erista-t1t-run1.txt`). Bit 14 is clear in the first and set in the second, so D0-S's gate would have gone on to read SDMMC1
  `0x700B0024` and `0x700B002C`. T1-T never read SDMMC1, so those two reads have no EL1 result either.
- The `setexpr` code and U-Boot's MMU map were not traced. A static search of `bl33.bin` for the command table entry found no
  pointer to the name, so the load path and the mapping of `0x60000000` and `0x700B0000` are unread. The cause is open.

## Design

The U-Boot script keeps the steps that ran (board gate, FDT framebuffer properties, DTB copy, loader write, `bootm prep`, the
`D0-S FRAMEBUFFER PROPERTIES` echo) and makes no load from a device address (D0's RAM reads stay). The reads move to the loader, in `boot/diag/loader/s2*.c`:

- `switch_s2_run` reads `RST_DEVICES_L` and `CLK_OUT_ENB_L` first, then `0x700B0024` and `0x700B002C` (16 bit) only if bit 14 says the clock is on
  and the reset is off.
- The allowlist is those four addresses, each with its width: three 32 bit reads and Clock Control (`0x700B002C`) as a 16 bit read. A wrong width is refused by the gate. Timeout Control, Software Reset and the data port are not read. There is no write function in the stage.
- The loader screen shows each read as a BEGIN line and a RESULT line, then the 15 s countdown and the PSCI reset of every diagnostic loader. It has no D0-R reservation or memory report.

SDMMC1 is the new risk. A present-state read of a clocked controller is ordinary, but nothing here has done it at EL1. The v2 screen
prints the CAR words before the SDMMC1 reads so that a hang there still leaves the CAR result on the screen.

U-Boot checkpoints (a `sleep` and an echo around each read) would narrow the old hang, but each failed read costs a run and a power
hold. Not planned.

## Done offline

`//boot:diag_state_v2` (iOS Diag State V2, entry `ios-diag-state-v2`, `diag_state_v2` line of `erista-diag-image-hashes.txt`),
its script `boot/diag/state/u-boot-script-state-v2.txt` and the loader files `s2.c`, `s2_regs.c`, `s2_gate.c`, `s2_access.c`,
`s2_report.c`, `s2_settle.c`. The script is D0's plus the title line and the framebuffer echo; it makes no load from a device address
and keeps D0's RAM reads. The loader draws a BEGIN line before each read and a RESULT line after it, each followed by `dsb sy` and a
100 ms wait on the counter, 11 lines in all; a line that did not settle (no screen, a counter that is not 19.2 MHz or does not advance)
stops the run before the next read with stop `0802`. The SDMMC1 registers are read only behind the CAR bits, Clock Control as a 16 bit `ldrh` (hekate declares `clkcon` as `vu16`, Linux reads it with `readw`; no source reads it as a word). Host tests: `s2_test` (20),
`diag_state_v2_test` (script, inspection, real link, hashes); the static audit is `boot/diag/erista-s2-mmio-audit.txt`. The run sheet
is `ERISTA_D0S_V2_RUN.md`. Not run: Bazel, the Mac build, any hardware. D0-S v2 stays HOLD.

## Result

The first run passed: `S2 COMPLETE. READS 4. STOP 0000`, hekate back by itself, 35 C at the end (`ERISTA_D0S_V2_RESULT.md`). The register values
were first reported without values; the values followed at 21:40 JST (`ERISTA_D0S_V2_RESULT.md`). D0-S v1's hung instruction stays unidentified.
