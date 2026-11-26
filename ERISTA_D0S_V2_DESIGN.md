# D0-S v2

D0-S (the first one) hung on the first U-Boot MMIO read. `boot/diag/erista-d0s-run1.txt` has the run. D0-S stays as it is and is
not run again. v2 is a separate image and a separate script. Nothing is approved to run: `boot` 0, feature mask 0, every HOLD.

## What the run shows

The script printed the framebuffer line, then the next command was `setexpr.l state_car_reset *0x60006004`. The screen never
moved on. Three places fit that: the read of `0x60006004`, the read of `0x60006010`, or a later step whose text the video console had
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
`D0-S FRAMEBUFFER PROPERTIES` echo) and has no `*ADDR` load at all. The reads move to the loader, in `boot/diag/loader/s2*.c`:

- `switch_s2_run` reads `RST_DEVICES_L` and `CLK_OUT_ENB_L` first, then `0x700B0024` and `0x700B002C` only if bit 14 says the clock is on
  and the reset is off.
- The allowlist is those four addresses. There is no write function in the stage, the same shape as T1-T.
- The loader screen shows the four words, or why SDMMC1 was skipped, before the D0-R report and the 15 s reset.

SDMMC1 is the new risk. A present-state read of a clocked controller is ordinary, but nothing here has done it at EL1. The v2 screen
prints the CAR words before the SDMMC1 reads so that a hang there still leaves the CAR result on the screen.

U-Boot checkpoints (a `sleep` and an echo around each read) would narrow the old hang, but each failed read costs a run and a power
hold. Not planned.

## Done offline

`s2.h`, `s2.c`, `s2_regs.c` and `s2_test` (7 tests: order, SDMMC1 only behind the clock and reset bits, the T1-T words, the exact
allowlist). Not done: the image, its screen lines, the script, the inspection mode and the hash line. Each needs approval and a Mac build.
