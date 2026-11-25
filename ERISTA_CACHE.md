# Cache maintenance from `bootm loados` to `go` and to the EL1 loader

Investigation of D1's "does the exporter's code reach instruction fetch" question. Static reading of the pinned `bl33.bin`
(`b492a401...`, U-Boot 2024.NX02.b201801, offsets are in the unrelocated image, so add `0xAA000000` for the addresses the
D1 script uses). No code was changed and nothing was run on hardware; every row below is a reading of the binary.

## Answers

| Question | Finding | Evidence |
| --- | --- | --- |
| Are the caches on while the script runs? | Yes, by static reading. `initr_caches` (`0xFE90`) is in `init_sequence_r`; it calls `enable_caches` (`0x17D4`): `icache_enable` (`0x1760`, invalidates the I-cache, sets `SCTLR.I`), then `dcache_enable` (`0x1478`, invalidates the D-cache, builds the tables with `mmu_setup`, sets `SCTLR.C`). | the init list at file offset `0x4F030`; no `dcache_disable` is reachable from `bootm loados`, `bootm prep` or `go` |
| What does it map? | The U-Boot `mem_map` (file `0x4D128`): `0x0..0x80000000` device (attrs `0x60000000000000`), `0x80000000..` normal write-back inner-shareable (attrs `0x310`), executable. `0xA7000000`, `0xA0000000`, `0xAA000000` are in the second region. | the table |
| Does `bootm loados` clean the D-cache? | Yes. `bootm_load_os` copies (CPU `memmove`, cached), then `flush_cache(load, ALIGN(len, 64))` (`0x2390`), a tail call into `flush_dcache_range` (`0x1474`), which is `__asm_flush_dcache_range` (`0x18BC`): `dc civac` per line over the range, then `dsb sy`. Clean and invalidate to the point of coherency, which is stronger than the `dc cvau` an instruction-fetch sequence needs. | call at `0x10964`, with `x0 = load`, `x1 = (load_end + 0x3F - load) & ~0x3F` |
| Does it invalidate the I-cache? | No. `invalidate_icache_all` (`0x174C`) is called only from `icache_enable` and `cleanup_before_linux`. `ic iallu` appears once more, in the region-attribute change. So after `loados` the I-cache may still hold lines for `0xA7000000`. | callers of `0x1920` and of `0x174C` |
| What does `go` do about caches? | Nothing. `do_go` (`0x4900`) prints, calls `do_go_exec` (`0x48F0`) and prints the result. No flush, no invalidate, no cache disable. | `0x4900..0x497C` |
| What is `0xAA001920`? | `__asm_invalidate_icache_all`: `ic ialluis; isb; ret`. This is what the D1 script runs to make the loaded exporter visible to the fetch side. | `0x1920` |
| What is `0xAA001A64`? | Not a function start. It is the tail of `__asm_invalidate_tlb_all`'s EL1 arm (`tlbi vmalle1` is at `0x1A60`); entered here it executes `dsb sy; isb; ret`, a barrier. It supplies the `dsb` that should follow `ic ialluis`. | `0x1A24..0x1A6C` |
| Why do both helpers return 1? | `do_go` passes `w0 = argc - 1` to the entry (`mov w0, w1` at `0x48F4`), which is 1 for `go <addr>`. Neither helper touches `x0`, so each returns 1. `do_go` returns `!!rc` (`cmp x0, #0; cset w19, ne`), so the command fails and the script's `if go ...` takes the else branch. A helper whose `go` reported success would be some other code. | `0x4944..0x4960` |

The order the script runs them in matters and is right: the D-cache clean (inside `loados`), then `ic ialluis; isb`, then
`dsb sy; isb`, then `go`. Nothing between the helpers and `go 0xA7000000` writes to `0xA7000000..0xA7010000` or runs a
cache operation on it: the loader's `loados` writes `0xA0000000`, `read mmc` uses the bounce-buffer flush and invalidate on
its own buffer, and `crc32` only loads.

## What the CRC32 checks do and do not see

The checks after `loados` and before `go` read the exporter through the D-side. They show that memory, as the CPU sees it,
holds the pinned bytes. They cannot show what the I-cache holds. That part rests on `ic ialluis` and on the exporter area
never having been executed before. It is what the helper is for, and it is unobserved.

## Handing the SD reference page and the record to the EL1 loader

The status block (`0xA700F000`), the reference page (`0xA001F000`), sector 0 and the exporter's record are all written by
the CPU with the D-cache on (`mw`, `crc32`, the bounce-buffer copy of `read`, the exporter's stores). `bootm go` for the
loader (OS type 5) takes the Linux path: `announce_and_cleanup` calls `cleanup_before_linux` (`0xCE8`): `icache_disable`,
`invalidate_icache_all`, `dcache_disable` (`0x1588`), which clears `SCTLR.M` and `SCTLR.C` and then runs
`__asm_flush_dcache_all` (`0x18AC` into `0x184C`): `dc cisw` over every data or unified level from `CLIDR_EL1`, then
`dsb sy; isb`. After it no dirty line is left on this core, and the EL1 loader reads with the MMU off (device type, so no
cache is consulted either). D0 and D0-R ran this path on the console and the loader read the DTB copy the script had written
with the CPU, which is the same mechanism for the same kind of data. D1 adds addresses, not a new mechanism.

The loader's own bytes at `0xA0000000` get the `loados` clean, and `cleanup_before_linux` invalidates the I-cache after it.

`read mmc 1` is DMA. The Tegra MMC driver goes through the bounce buffer (`bounce_buffer_start` at `0x12C04`, `bounce_buffer_stop` at `0x12C88`): a flush of the buffer before
the transfer, an invalidate after. The user buffer at `0xA001F200` is 64 byte aligned and 512 bytes long, so it is used
in place. The `mw.b` clear before it leaves dirty lines on `0xA001F000..0xA0020000`; the flush before the transfer cleans
them first, so a later write-back cannot overwrite the DMA data.

## What stays unverified

- That the init list reaches `initr_caches` on this console, and so that the D-cache is on. This U-Boot has no `dcache` or
  `icache` command (no string for them), so it cannot be asked from a script. Without the D-cache the sequence is still
  correct: the copy is then a direct write.
- The set/way clean covers this core's caches. Other cores are not known to be parked in a state that has no dirty line on
  these addresses (BL31 was read for hashes only).
- `ic ialluis` is an inner-shareable broadcast; whether the instruction fetch of the exporter sees nothing stale is shown only
  by the exporter running.
- A stale I-cache line would show as a hang or a fault at `go 0xA7000000`. That is the case `RUN_CHECKLIST.md` names.

No `dc cvau`, `ic ivau` or range helper is added: `loados` already cleans with `dc civac`, and a by-range I-cache invalidate
would be a new routine in RAM that the script then has to trust. If the first run hangs at that `go`, the next step is a
review, not a patch.
