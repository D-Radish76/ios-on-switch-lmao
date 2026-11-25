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

## Mac run of `hekate_bin_check.py` (2026-11-26, reported by the user, not run here)

| File | SHA-256 | Size |
| --- | --- | --- |
| hekate (`hekate_ctcaer_6.5.5.bin` as the user named it) | `09752abd207803708add6e22151e94b71ffbaaff769d1bad759584b76ef6f05e` | 110208 |
| `nyx.bin` | `309f58f934b50c4ae990f8dbb02b7106c16b9391bb1e516d66f1aa48871d1e6d` | 477565 |

Output: hekate, all six I2C table rows not found; Nyx, the whole table at `0x71b74`; the literal `0x00030003` in neither.
Not yet known: whether these files are the ones in the release zip (the zip's hash and the card copies were not part of
the report), and which card file started hekate. Equality with the release zip and with the card stays unverified. The
size of 110208 bytes has no earlier record to compare with.

The check is a byte search. It says nothing about which code runs.

### Why the literal is absent (both files)

Expected from the compilers, so no finding by itself. In ARM mode (Nyx, no `-mthumb`) GCC builds `0x30003` as
`mov rd, #0x30000` then `orr rd, rd, #3`, because both halves are encodable immediates, so no literal word exists. The
checker now looks for that `mov` (`arm_mov_30000`). The Thumb build (hekate, `-mthumb`) has no `movw` on ARMv4T; it can
use a literal pool word or a shift sequence, and a pool word would have been found at a 4 byte offset. The checker cannot
tell which. So the absence says nothing about whether `hw_init` writes `0x30003`; it needs the disassembly.

### Why the table may be absent from hekate

From the source and the Makefiles: both builds compile the same `clock.c` and the same table. The flags differ (hekate
`-mthumb -fno-inline`, Nyx ARM mode with inlining; both `-O2`, `-fdata-sections`, `--gc-sections`). The table is data
indexed by the runtime argument of `clock_enable_i2c`, so the instruction set does not change it, and GCC at `-O2` does
not clone a function for two different constants (hekate passes 0 and 4). The source therefore predicts the table in
both files. Candidates, none confirmed:

1. the file is not the 6.5.5 payload of the release (a different build, or another file in the zip);
2. the rows are in the file in another layout (an older struct, padding, or bitfield order in the Thumb build);
3. `hw_init` in this build does not use the table (a different `clock.c`).

What separates them needs the file: the identity strings the checker now reports (`hekate v6.5.5`, the `ICTC` magic, as
set by `main.c:1429` and the Makefile), a search of the hekate file for the single I2C source addresses and offsets as
literals (the checker now looks for `0x60006124`, `0x60006128`, `0x60006110` and `0x600060A8`), and a Thumb disassembly
of the code around any hit. In ARM mode the same words can be searched in Nyx beside the table that is there.

### Needed

Attach `hekate_ctcaer_6.5.5.bin` (110208 bytes) and `nyx.bin` (477565 bytes) as the Mac hashed them, or run the updated
`hekate_bin_check.py` and send its output. The disassembly (items 3 and 4 of the request) cannot be done from hashes.
Also useful: the zip's SHA-256 and the hashes of the card's `bootloader/update.bin` and the file that started hekate.

## Binaries received and read (2026-11-26)

The user sent both files in a zip and reports that their SHA-256 equal the card's `bootloader/update.bin` and
`bootloader/sys/nyx.bin`, taken from the official release. Re-hashed here: they equal the values above. The zip's own
SHA-256 was not part of the report, so "from the official release" rests on the user's statement. Output of the checker on
the files: `boot/diag/erista-hekate-check.txt`.

### Findings (read from the bytes; nothing was run)

Confirmed:

- The hekate file is hekate: it holds the `ICTC` magic at `0x118` and `0x82b`, followed by the version bytes `'6' '5' '5'`
  and a fourth byte `0x31`. In the source the fourth byte is `BL_VER_RL`, and `Versions.inc` of the tag has
  `BLVERSION_REL := 0`. So the version word of this binary is not what the tag's `Versions.inc` produces. The release may
  be built with another `REL` (a file difference between the tag and what was built), or the tag differs; not decided.
- `i2c_init` is in the hekate file: two adjacent literals `0x00050001` and `0x00090003` at `0x5188`, `str rX, [r5, #0x6c]`
  (`I2C_CLK_DIVISOR`) and `movs r3, #0x84` with a store at `[r5, r3]` (`I2C_BUS_CLEAR_CONFIG`) in the code just before them
  (`0x514c` to `0x515e`). Nyx has the same two literals at `0x274b0`. This is the driver's `(5 << 16) | 1` and
  `(9 << 16) | BC_TERMINATE | BC_ENABLE`.
- The `_i2c_base_offsets` table `{0, 0x400, 0x500, 0x700, 0x1000, 0x1100}` is in Nyx (`0x72018`) and not in the hekate file.
  The literal `0x7000C000` (I2C base) is in neither.
- The I2C clock table of `clock.c` is in Nyx exactly (`0x71b74`). In the hekate file the first seven bytes of the I2C1
  and I2C4 rows are present (`0x17bf6`, `0x17c0c`), followed by `cf` where the source has the divider `03`; the I2C2, I2C3,
  I2C5 and I2C6 rows are not. Other row fragments sit near them in a layout that is not the source's 8 byte array
  (one tail matches the I2C2 or I2C3 row, the leading bytes do not). Whether this is a differently laid out table, code and
  data that only look alike, or a build from other source, was not decided.
- No literal for `PLLP_OUTB`'s value or any I2C source address is in the hekate file, and the only CAR literals are a
  few dozen (`0x60006000`, `0x60006300`, `0x60006308`, `0x6000632c`, ...). A constant tracker over the Thumb disassembly
  finds device accesses to PMC, CAR, `0x7000e4xx` and others, none to the I2C source offsets.

Inferred: the instruction set does not explain the absence of literals. In ARM mode (Nyx) a constant such as `0x30003`
is built with `mov` and `orr`; two such `mov rd, #0x30000` are in Nyx (`0x291ac`, `0x292dc`). In Thumb mode GCC may use
shifts and adds as well, which a literal search cannot see. The I2C base `0x7000C000` can also be built in registers.

Not shown: that `hw_init` in this hekate file writes the I2C1 and I2C5 sources, or does not. The disassembly has not been
followed to that code: the places where `i2c_init` is called, and the `clock_enable` code with its constants, were not
found by this pass. So item 3 of the request (I2C1 and I2C5 clock setup from the instructions) is open.

### The next step that can settle it

Build the tag and compare. If the Mac has devkitARM (`arm-none-eabi-gcc`), `git checkout v6.5.5 && make -j` makes
`output/hekate_ctcaer_6.5.5.bin` and `output/nyx.bin`. A byte for byte match with the release proves the source (and
the exact compiler) are what the release was built from; a difference in a few bytes (the version byte) shows what the
release changed; a large difference shows the compiler or flags differ. Then `hekate_bin_check.py` on both outputs
gives the same table layout questions with the source in hand. No compiler is installed here.
