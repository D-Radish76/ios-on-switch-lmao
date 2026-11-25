# BL31, U-Boot and the I2C clock source: offline reading

Question: who writes `00000003` into `CLK_SOURCE_I2C1` and `CLK_SOURCE_I2C5` between hekate's `hw_deinit` and the first
U-Boot read (P1 of T1-R)? Inputs are the pinned `bazel/pinned/firmware/bl31.bin` and `bl33.bin`, hekate 6.5.5 source and
Linux source. `tools/package/a64_scan.py` reads both binaries, `boot/diag/erista-bl31-scan.txt` and
`erista-bl33-scan.txt` keep the result, `tools/package/test/a64_scan_test.py` tests them. Nothing was run on hardware.

The scanner tracks constants through straight code and reports device accesses and call arguments. It does not follow
values through memory or through calls other than ones that return a device address, so every "none" below is a
statement about what the scanner resolves.

## A. BL31

Confirmed from the binary (BL31 linked at 0, 45655 bytes, SHA-256 `dea0ecd4...e2b4`):

- 60 resolved accesses with a device address: 33 CAR (11 loads, 22 stores) and 27 PMC. None is an I2C block.
- CAR offsets touched: 0x28, 0x30, 0x74, 0x78, 0x28c, 0x290, 0x2a8, 0x300 to 0x310, 0x358 to 0x364, 0x42c, 0x430,
  0x454 (`erista-bl31-scan.txt`). None is 0x124, 0x128, 0x198, 0x1b8, 0x3c4 or 0x65c.
- No 32-bit word in the file equals any of the six source addresses, and no `mov` has the low half of one, so there is no
  address table and no instruction that builds one in a register.
- All accesses use constant addresses built with `mov` and `movk` in the function that uses them. The six call lines
  the scan prints are leftovers: spot checks of the callees (0x26c4, 0x7040, 0x7160, 0x68e4, 0x6628) show code that is
  not a read-modify-write helper taking an address.

Inferred: BL31 has no clock init table and no generic MMIO helper that a CAR address could reach, so it does not write the
source registers. Nothing in it is specific to Erista versus Mariko that the scan could show.

Unverified: data-dependent writes (an offset loaded from memory) and code reached only through a function pointer.

## B. U-Boot

Confirmed from `bl33.bin` (linked at 0xaa000000):

- The embedded control FDT has six `nvidia,tegra210-i2c` nodes and a `clock@60006000` node. The strings
  `assigned-clock`, `assigned-clock-rates` and `assigned-clock-parents` occur zero times in the whole binary, and in
  `bl31.bin`. So no device tree property sets I2C1, I2C5 or any other bus at probe time.
- The CAR accesses it resolves are offsets 0x50, 0x88, 0x8c, 0xa0, 0xa4, 0xa8, 0xdc, 0x1d4 and 0x55c. None is a source
  register. No word in the file equals a source address.
- The periph id to register function at 0xaa0034c0 is a per-id table. The source setters 0xaa002ae0 and 0xaa002b3c
  keep the top 16 bits of the register and replace the low 16 (`and w3, w3, #0xffff0000; orr w3, w3, w24`). They never
  write the source field, so even a probe of I2C5 would leave the field as it found it.
- Callers of 0xaa002b3c: 0xaa0215bc and 0xaa021630, inside the I2C driver's speed setup. Both pass parent index 2. The first is
  taken for bus speeds up to 400 kHz and passes 24727273 Hz (408 MHz divided by 16.5); the second passes the bus speed
  itself. Which bus id reaches them is not shown by the scan.
- The three scan hits with a periph number (`0x36`, `0x43`, `0x2f`) are not clock calls: 0xaa012ff0 takes a character
  (0x43 is `C`) and 0xaa01c9b8 is a device lookup by uclass id.

Inferred: only I2C5 is enabled in the FDT (400 kHz, clock id 0x2f) and its source field is never written by U-Boot, so
U-Boot is not the writer of `00000003` on I2C1 or I2C5. P1 reads it before any I2C5 probe could matter, and P2 and P3 do
not differ from P1, so a probe in between left no trace in the field.

Unverified: whether I2C1 could be reconfigured while disabled (the driver does not run for a disabled node, which is
how U-Boot's driver model works, not something read here), the unnamed code between the setter and its callers, and the
clock id to register mapping beyond id 0x34 (the table was read at one entry).

## C. Clock arithmetic

Confirmed, hekate source: `CLK_I2C_SRC_DIV(d)` is `d - 1`; I2C1 gets `C0000003`, I2C5 `C0000000`. The measured I2C1
divisor word is `00050001`: Linux's `I2C_CLK_DIVISOR_STD_FAST_MODE` is bits 31:16 and `HSMODE` bits 15:0, and its T210
`clk_divisor_hs_mode` is 1, so the layout fits (hekate writes 5 for the first field).

Confirmed, Linux `clk-tegra210.c`: PLLP `{38400000, 408000000, n 85, m 8, p 1}`, so `PLLP_BASE 48115408` is M 8, N 85,
P 1, 408 MHz when the PLL is locked (bit 27 is set in the word).

Inferred, source field: the I2C mux is `mux_pllp_clkm` (Linux), index 0 `pll_p`, index 3 `clk_m`. `C0000003` has bits 31:30
equal 3, `clk_m`; `00000003` has them 0, `pll_p`. Read as hekate's 3 bit field (bits 31:29) the same words are 6 and 0.

Inferred, divider: the field is read as an integer, `divider = field + 1`. hekate's comments (19.2 MHz / 4 = 4.8 MHz is 100 kHz
on I2C1, 19.2 MHz / 1 is 400 kHz on I2C5) come out exact only that way; the 7.1 fractional reading would give 160 kHz on
I2C1. So `00000003` is 408 / 4 = 102 MHz, against hekate's 4.8 MHz.

Inferred, bus rate: Linux uses `module / ((TLOW + THIGH + 2) x (divisor + 1))`, with T210 `TLOW` 4 and `THIGH` 2 as software
values. With the measured divisor 5 and those timing values the bus would be 102 MHz / 48 = 2.125 MHz instead of
100 kHz. `INTERFACE_TIMING_0` and `_1` (0x7000c094, 0x7000c098) were not read, so the timing values are unverified.
Linux only writes them and never reads them back. No read side effect is documented in what was read; the reference
manual was not available, so a read is not shown to be harmless.

Inferred, `PLLP_OUTA 00000003` and `PLLP_OUTB 00030003`: Linux's PLL output helper uses `enb_bit_idx` and `rst_bit_idx`
and sets both to enable; for PLLC_OUT in `clk-tegra114.c` the call is enable bit 1, reset bit 0, with the 8 bit ratio at
shift 8. Read the same way, OUTA has output 1 enabled with ratio 0, OUTB has outputs 3 and 4 enabled with ratio 0. The
T210 `PLLP_OUTA`/`OUTB` layout itself was not found in the Linux files read, so this is carried over from Tegra114 and
the earlier note that bit 0 is the enable is withdrawn. The module clock above does not depend on these words, because
the I2C mux takes `pll_p` itself.

TMP451 (TI SBOS686A, read earlier): fast mode to 0.4 MHz; high-speed to 2.5 MHz only after a master code byte. The T1 v2
sequence equals hekate's `_i2c_send_normal` and sends no master code. A 2.1 MHz bus is outside what the part accepts in
fast mode. That fits the NOACK of T1 v2 and does not prove it: a part in shutdown and a bus fault are not excluded.

## D. Reading the registers just before the hand-off

The writer lies somewhere after `hw_deinit` and before U-Boot's P1. hekate's `hw_deinit` itself (source read) touches the
display, the BPMP clock rate, `tmp451_end`, `minerva_deinit`, the MMU and two enable bits; it writes no I2C source.
`bpmpfw.bin` and BL31 and U-Boot have no resolved writer either (`ERISTA_BPMPFW.md`, A, B). What remains is hekate's
`_l4t_late_hw_config` (it handles PMIC bits, no clock access found), data-dependent code in the three binaries, and
hardware behavior.

| Point | What it separates | Cost and risk |
| --- | --- | --- |
| Nyx "Payloads" (`hw_deinit(false)`, then the payload) | the state hekate leaves, from everything after | A payload binary that reads CAR. It is a different path from `launch_l4t` (`hw_deinit(sld_type)`, late config), so it brackets the writer and does not show the exact state. |
| `launch_l4t` just before `BPMPFW_ENTRYPOINT` | the whole hekate side from BPMP-FW onward | A rebuilt hekate. It changes the boot chain the user trusts, so it carries the most risk and the clearest answer. |
| First instructions of BL31 | BPMP-FW from BL31 | Replaces a pinned binary and runs at EL3, with only reads. Narrower than the two above. |
| U-Boot P1 (exists) | nothing before it | Already measured: `00000003`. |

If a payload is ever allowed, the first one is the cheapest cut: if I2C1 already reads `00000003` there, the writer
is in hekate's own deinit or earlier, which the source says does not touch it, so a mismatch with the source would itself
be the finding. If it reads `C0000003`, the writer is in the l4t late path, BPMP-FW, BL31 or the hand-off. Design only;
no payload exists and none is proposed for now.

## Open

- The writer is still unidentified. Static reading finds none in BPMP-FW, BL31 or U-Boot, and none in hekate's deinit.
- Whether the register could change by itself (a reset value of the divider, a default after `hw_deinit` clears
  something) is not shown. The divider field kept hekate's value while the two source bits read 0.
- `RST_L` bit 22 (T1 v2 `049102CA` against T1-R `04D102CA`) is still unresolved.

`boot` stays 0. T1-R re-run, T1 v3, T2 to T4 and D1 hardware boot are on hold. No clock, firmware or payload change.
