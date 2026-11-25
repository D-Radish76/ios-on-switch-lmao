# hekate 6.5.5: source against the release binary

## Source and release

Confirmed with `git` against the project's repository: tag `v6.5.5` is `9593272c54f8dc127a5a25f91e1ac90e1be6205d`. The
tree read for every hekate note here is `8e1392df4569ac501a115867056b13c361429e9e`, one commit after it (2026-11-11,
"hos: fix 23.5.0 FAT32 FS nogc offset"). `git diff --name-only v6.5.5 8e1392d` lists one file,
`bootloader/hos/pkg2_patches.inl`. So `hw_init.c`, `clock.c`, `i2c.c`, `l4t.c`, `main.c`, `nyx.c`, `gui.c`, `di.c`,
`memory_map.h` and `util.h` are the release's.

## Release binary

Not checked. The release page is not reachable from this session (HTTP 403). Needed from the Mac: the zip the card was
made from (recorded SHA-256 `e3c6613f...5df56`) and the card's own copies. Commands:

```
shasum -a 256 hekate_ctcaer_6.5.5_Nyx_1.9.5.zip
mkdir /tmp/hk && unzip -o hekate_ctcaer_6.5.5_Nyx_1.9.5.zip -d /tmp/hk
shasum -a 256 /tmp/hk/hekate_ctcaer_6.5.5.bin /tmp/hk/bootloader/sys/nyx.bin
shasum -a 256 /Volumes/<card>/bootloader/update.bin /Volumes/<card>/bootloader/sys/nyx.bin
python3 tools/package/hekate_bin_check.py --hekate /tmp/hk/hekate_ctcaer_6.5.5.bin --nyx /tmp/hk/bootloader/sys/nyx.bin
```

The zip's layout and the name of the file that started hekate on the card (`payload.bin`, `update.bin` or another) are
as the earlier notes say and have not been seen here. Send the two binaries, or the script's output, and the card's hashes.
`hekate_bin_check.py` prints each hash, looks for the six row I2C clock table of `clock.c` (8 bytes per row,
`reset, enable, source, index | 6 << 5, divider`) and for the literals `0x00030003`, `0xED000000`, `0xC0000000`,
`0x40010000`, `0x40007000`. A hit shows that the bytes are in the file, not that code uses them. Showing the
I2C1 and I2C5 writes in the machine code needs a disassembly pass over the real binary, which comes after the hashes match.

## What the source says

- `hw_init` (`hw_init.c`): `clock_enable(&_clock_pwm)` at line 404, `PLLP_OUTB = 0x30003` before that,
  `clock_enable_i2c(I2C_1)` and `(I2C_5)` at 417 and 418, `i2c_init(I2C_5)`, regulators, `i2c_init(I2C_1)`.
- `clock_enable` (`clock.c:163`): reset set, clock gated, `source = (clk_src << 29) | clk_div`, clock enabled, 2 us,
  reset released, then a read of the reset register. The source register is never read back there. Table rows: I2C1
  `C0000003`, I2C5 `C0000000`.
- Path from Nyx to the l4t launch: `_launch_hos` (`gui.c:903`): `sd_end`, `hw_deinit(false)`, jump to
  `nyx_str->hekate`; `_start` relocates the image to `0x40008000`, `_real_start` sets the stack and clears bss,
  `_irq_setup` branches to `ipl_main` (`exception_handlers.S:115`), which calls `hw_init` (`main.c:1440`,
  "compatible with consecutive reruns"), then the autoboot path (`main.c:973`), `launch_l4t`, `hw_deinit` (`l4t.c:1168`),
  the late config, and the jump to `BPMPFW_ENTRYPOINT` (`l4t.c:1186`).

## Do written values hold?

Not shown for I2C. For PWM it is shown: `hw_init` writes `CLK_SOURCE_PWM` the same way (`(6 << 29) | div`, `C0000004`)
and T1 v2 read `C0000004` at the loader, after the whole chain. The I2C1 and I2C5 words written by the same function did
not survive to P1. So retention of a CAR source write across that chain is the normal case, and I2C1 and I2C5 are
the exceptions. That points at something that touches those two (and not the PWM or the other four I2C sources), and
does not say what. It is an observation, not an identification. Nyx reads the TMP451 on I2C1 and showed a temperature,
which suggests I2C1 works at that point; it does not show its source word.
