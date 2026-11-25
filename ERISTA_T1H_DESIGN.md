# T1-H: read-only CAR payload from Nyx "Payloads" (design only)

Nothing here is implemented. T1-R re-run, T1 v3, T2 to T4 and D1 hardware boot are on hold. `boot` stays 0. No firmware
is replaced and no measured register is written.

Sources: hekate commit `8e1392d` (`Versions.inc` 6.5.5, Nyx 1.9.5; a partial, sparse checkout of the project's git history)
and the earlier reads in `ERISTA_T1R_RESULT.md`, `ERISTA_BPMPFW.md` and `ERISTA_BOOTCHAIN_CAR.md`.

## Correction to the earlier reading

The T1-R words are not all a change of the source bits. I2C1 kept hekate's divider (3) and lost the source bits
(`C0000003` to `00000003`). I2C5 changed both: hekate writes `C0000000`, the run read `00000003` (divider 0 to 3, source
3 to 0). One cause for both is not shown. Nothing below treats them as the same event.

## What the Nyx Payloads path does

Confirmed from source (`nyx/nyx_gui/nyx.c` `launch_payload`, `bdk/soc/hw_init.c`, `bdk/display/di.c`):

1. Nyx reads `bootloader/payloads/<name>`, refuses more than 0x30000 bytes, calls `sd_end`, copies the file to
   `0xC0000000 + 0xA0`, copies a 0x94 byte relocator (the head of hekate's own image, from `nyx_str->hekate`) in front of
   it, and patches four words in it: start `0x4000FF60`, stack `0x40007000`, end, entry `0x40010000`.
2. It calls `hw_deinit(false)`. Nyx is built with `BDK_HW_EXTRA_DEINIT`, so this does: `display_end` and
   `clock_disable_host1x` (screen off, DSI LDO0 off through the PMIC), `bpmp_clk_rate_set(NORMAL)`, `vic_end`,
   `tmp451_end` (TMP451 to shutdown, an I2C1 write), `fan_set_duty(0)`, `touch_power_off`, `jc_deinit`,
   `regulator_5v_disable`, `minerva_deinit` (DRAM 204 MHz), `bpmp_mmu_disable`, the arbiter reset, and the AHUB and APE
   clock enables.
3. It jumps to `0xC0000000`. The relocator copies the payload to `0x40010000` and jumps there.

Inferred from the same source: the CPU is the BPMP (ARM7TDMI, ARMv4T; hekate builds with `-march=armv4t -mthumb-interwork`),
entered in ARM state with SP at or near `0x40007000`. The privileged mode was not read; the payload would record CPSR
first. IRAM `0x40010000` to `0x40040000` is the payload's room (hekate's own image sits at `0x40008000` and is
overwritten). There is no MMU (`bpmp_mmu_disable`), no cache, and no exception vector set up by the payload.

Not shown: the exact relocator code in the shipped `nyx.bin`; whether the SP at entry is the patched stack.

## What T1-H can and cannot show

The l4t entries that were measured are launched from Nyx's "More configs" button, which is not this path:

| Path | Sequence before the register read |
| --- | --- |
| l4t from Nyx (T1-R) | Nyx `hw_deinit(false)` (with the extra deinit above), jump to hekate main, `hw_init` again (rewrites I2C1 and I2C5 sources), menu code, `launch_l4t`: hekate `hw_deinit` (no extra deinit), late config, BPMP-FW, BL31, U-Boot P1 |
| T1-H from Nyx Payloads | Nyx `hw_deinit(false)`, payload |
| T1-H from hekate's own text menu (`nogui`) | hekate `hw_init`, `hw_deinit(false)` (no extra deinit), payload |

So a Nyx-launched T1-H reads the state before hekate's second `hw_init`, and `clock_enable` writes
`(6 << 29) | divider` to the source registers there (confirmed, `clock.c:163`; `hw_init.c:417-418`). Nothing T1-H reads
can show what happens after that point. What it can show is whether the registers hold hekate's values while Nyx runs.
Nyx reads the TMP451 on I2C1 and showed 38.7 C after the T1-R run, so I2C1 is most likely `C0000003` there; that is an
inference, not a measurement. A different word would be a surprise worth a second look. The same word does not clear
hekate, and it does not move the writer into any one stage: the writer is after the second `hw_init` either way, because
that function writes both registers.

The hekate-menu variant is closer to the l4t path (one `hw_init`, then `hw_deinit`, then the next stage). It needs the
text menu, which is a change to the card's `hekate_ipl.ini` (`nogui`). That is reviewer's call and not part of this design.
The comparison with T1-R is on the 16 words only; the two paths differ in the extra deinit, in the late config, and in
all of BPMP-FW, BL31 and U-Boot. The closest cut of those stages remains a patched `launch_l4t` or a BL31 stub, both of
which replace firmware and are excluded here.

## What it reads

Read only, 32-bit loads from CAR (`0x60006000` base), the order of T1-R P3:

| Register | Address | Hekate's word (source) | T1-R value |
| --- | --- | --- | --- |
| `RST_DEVICES_L` | `0x60006004` | none (T1 v2 `049102CA`, T1-R `04D102CA`, bit 22 unresolved) | `04D102CA` |
| `CLK_OUT_ENB_L` | `0x60006010` | none | `9802D131` |
| `CLK_SOURCE_I2C1` | `0x60006124` | `C0000003` | `00000003` |
| `CLK_SOURCE_I2C5` | `0x60006128` | `C0000000` | `00000003` |
| `CLK_SOURCE_I2C2` | `0x60006198` | not written by hekate | `00000000` |
| `CLK_SOURCE_I2C3` | `0x600061B8` | not written by hekate | `00000000` |
| `CLK_SOURCE_I2C4` | `0x600063C4` | not written by hekate | `00000000` |
| `CLK_SOURCE_I2C6` | `0x6000665C` | not written by hekate | `00000000` |
| `CLK_SOURCE_PWM` | `0x60006110` | `C0000004` (T1 v2 read it) | not read in T1-R |
| `PLLP_BASE` | `0x600060A0` | locked by hekate | `48115408` |
| `PLLP_OUTA` | `0x600060A4` | none | `00000003` |
| `PLLP_OUTB` | `0x600060A8` | `00030003` (`hw_init.c:393`, "PLLP_OUT3/4 408 MHz") | `00030003` |

`PLLP_OUTB` equal to hekate's own constant confirms the OUTB decode (out3 and out4 enabled, ratio 0). Two more read-only
loads help the comparison and are listed for review: `HIDREV` (`0x70000804`, Erista or Mariko) and the microsecond timer
(`0x60005010`, elapsed time). CPSR is read with `mrs`. No I2C register is read (that block hangs when its clock is off;
T1-R guarded those reads, T1-H has no need for them).

Output record: a magic word, a version, the 12 words in this order, `HIDREV`, CPSR, the timer value at start and end, and a
CRC32 (not tamper protection), all as 8 digit hex so that it prints next to T1-R's lines.

## Showing the result

The screen is off when the payload starts, and turning it on needs more than framebuffer writes. Confirmed from
`bdk/display/di.c`: `display_init` sets LDO0 to 1.2 V and enables it through the PMIC (I2C5 transfers), enables and
resets MIPI_CAL, DSI and DISP1, sets two clock sources, starts PLLD, writes `PMC_IO_DPD_REQ`, GPIOs and pinmux for the
panel and backlight, and the backlight code calls `clock_enable_pwm`, which writes `CLK_SOURCE_PWM`, one of the measured
registers. Writing the old framebuffer (`0xF5A00000`, `0xF6200000`) shows nothing because nothing scans it out.

| Option | Writes beyond RAM | Verdict |
| --- | --- | --- |
| A. Re-initialize the display in the payload | I2C5 PMIC transfers, CAR clock and reset writes, PLLD, PMC, GPIO, pinmux, PWM source | Breaks the I2C ban and writes a measured register. Not proposed. |
| B. Store the record in DRAM, return to hekate, show it with a later read-only entry | RAM only | Proposed. |
| C. Write a file to the SD card | SDMMC init, CAR writes, the SD rail through the PMIC (I2C), pinmux | Not proposed. |

Option B in detail. The payload writes the record to a fixed DRAM address. A second entry shaped like T1-R (its script
already uses `md.l`) prints those words on the U-Boot screen with the read-only command set T1-R uses, beside T1-R's own
P1 lines. The address is not chosen here. It must lie outside everything hekate, Nyx and the l4t path use. The memory
map header leaves `0xE0000000` to `0xE4FFFFFF` unused (`DRAM_LIB_ADDR` is commented out; the 252 MB chainloader area is
used only when chainloading), and `0xA0000000` and `0xAA000000` hold the loader and U-Boot. It needs an audit against
the pinned U-Boot's memory map (`bdinfo`) and the D0-R reservation tables before any use. Survival of DRAM across
hekate's restart is expected (no reset happens) and not shown.

## Returning to hekate

The payload cannot use the return the existing diagnostics use (U-Boot's PSCI reset into BL31's reboot-to-payload); that
is BL31's, and no BL31 runs here. A PMC reset leaves the console at the BootROM, not hekate. So the return replicates
what Nyx itself does in `reload_nyx` and `_launch_hos`: set hekate's boot config, then jump to the hekate image saved
in DRAM.

- The saved image is `nyx_str->hekate`, at `0xED000000 + 8 + 0x8000 = 0xED008008` (`nyx_storage_t`: `version`, `cfg`,
  0x8000 reserved bytes, then `hekate[0x30000]`). The boot config is at that address plus 0x94 (`b_cfg` in `nyx.c`).
- Stale config is a hazard: a previous launch from Nyx left `BOOT_CFG_AUTOBOOT_EN` with an entry index in that config.
  A jump without resetting it could autoboot the same diagnostic entry again. `reload_nyx` sets `boot_cfg` to
  `BOOT_CFG_AUTOBOOT_EN`, `autoboot` 0, `autoboot_list` 0, `extra_cfg` 0, which makes hekate load Nyx again. The payload
  would write exactly those four fields and nothing else there.
- Hekate then runs `hw_init` again ("compatible with consecutive reruns", `main.c`). That rewrites the clock sources, so
  every measured value must already be in RAM before the jump.

Failure behavior: if the image header at `0xED008008` is not what the checks expect (to be fixed at review from the 6.5.5
binary), the payload does not jump. It stops in a `wfi` loop with the screen off, and the person holds the power button.
There is no on-screen message in that case. This is accepted for review, not hidden.

## Writes the payload would make

Everything else is a load. Listed for review, in the order they happen:

| Write | Where | Why |
| --- | --- | --- |
| Stack pushes | IRAM below the chosen SP | C or assembly calls |
| Result record, about 20 words | the audited DRAM address | Option B |
| `boot_cfg`, `autoboot`, `autoboot_list`, `extra_cfg` | `0xED008008 + 0x94` | Return, same four values as `reload_nyx` |
| Instruction fetch of the jump | `0xED008008` | Return |

No store to CAR, I2C, PWM, PMC, MC, EMC, SDMMC, GPIO, pinmux, display, fuse or any other device block. Verification
would follow T1-R: an inspector over the linked image that rejects a store whose address is not one of the three above,
requires each CAR read to be a single load with one caller, and a test over the allowlist.

## Fan, temperature, time

Nyx's `hw_deinit` already stops the fan (`fan_set_duty(0)`) and puts the TMP451 in shutdown before the payload runs, and
the l4t runs launched from Nyx start from the same state, so T1-H adds no thermal change. The payload itself is a
few dozen loads and stores (microseconds); the visible time is the black screen until Nyx is back, which is hekate's
`hw_init` plus a Nyx load from the SD card (the 24 s of the T1-R video include U-Boot and the loader stages; this path
has none of them). The 30 s human deadline is provisional, not a thermal value: past it, hold the power button. No
watchdog is armed; that would be a T4 item. There is no software guard against a hung load, and none is expected for CAR.

## Reset values and clock domains

Not available: the reset value of each register and the domain it sits in come from the Tegra X1 TRM, which was not
available here, and no other source read states them. Known from what was read: hekate writes the I2C1 and I2C5 sources and
nothing else of the six; the other four read `00000000` after the BootROM and everything since. The CAR block does not
depend on a peripheral clock, and reading it is not expected to hang. Unverified.

## Source and binary correspondence

The local tree reports `Versions.inc` 6.5.5 and Nyx 1.9.5 and is a blobless sparse clone; it has no tag, so equality
with the release is not shown. The card's `hekate_ctcaer_6.5.5.bin` and `nyx.bin` were not hashed in any earlier note (the
recorded hashes are `bpmpfw.bin`, `sc7entry.bin`, `sc7exit.bin`, `libsys_minerva.bso` and the zip). Before any
implementation: hash both files and the zip's copies, and check in the disassembly of `nyx.bin` that `launch_payload`
uses `0xC0000000`, `0x94`, `0x40010000`, `0x40007000` and `0xED000000` and that `hw_deinit` contains the extra deinit
calls, so the reading above applies to the binary that runs.

## Questions for the review

1. Is a Nyx-launched T1-H worth building, given that it reads the state before hekate's second `hw_init` and is expected to
   show hekate's own words? The hekate-menu variant and a BL31 stub are closer cuts; both need a decision on card or
   firmware changes.
2. Option B and its second entry, or accept a payload that records and returns with nothing shown.
3. The DRAM address for the record, after the audit.
4. Whether `HIDREV`, CPSR and the timer are acceptable reads.
5. A black screen with a power-button fallback and a 30 s deadline.
