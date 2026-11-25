# Memory diagnostic (D0-pre)

Two hekate entries that print what U-Boot reports about memory, one page each, then reset. They run before D0 so the
loader's destination can be judged against numbers read from the console instead of the OLED's.

| Entry | Shows |
| --- | --- |
| `iOS Diag Memory` | board (`t210b01`, `sku`), then `bdinfo`: DRAM banks, `relocaddr`, `reloc off`, `irq_sp`, `sp start`, `TLB addr`, `fdt_blob`, `boot_params` |
| `iOS Diag Memory FDT` | the control FDT's `/memory`, `/reserved-memory` and reserve map |

Each entry has its own `bl33.bin` (U-Boot plus one script) and `bl31.bin`. The scripts contain no address and no
command that writes, loads, boots or saves; the images carry no loader, DTB, exporter or kernel window. After 15 s each runs `reset`, the
same tail as `iOS Diag`, which landed in hekate on the Erista.

## Build

```sh
python3 tools/package/diag_memory.py build \
  --frame boot/diag/memory/u-boot-script.txt \
  --page1 boot/diag/memory/page-1-bdinfo.txt --page2 boot/diag/memory/page-2-fdt.txt \
  --boards kits/SwitchBoard/SwitchBoard.h --bl31 bazel/pinned/firmware/bl31.bin \
  --bl33 bazel/pinned/firmware/bl33.bin --out "$PWD/build/diag-memory"
```

Copy `bootloader/ini/ios-diag-memory*.ini` and `switchroot/ios-diag-memory*/` onto the SD card's FAT. Read each
`script.txt` first.

## Run

Charged battery, console on a table, attended: nothing here manages the fan or the temperature. Choose `More configs`,
then one entry; photograph the screen before the reset; repeat for the other. Hold the power button if a screen stays
unchanged for 20 s.

The pinned `bl33.bin`'s control FDT has no `/reserved-memory`, so page 2 should print a libfdt error for it.

## Safety

- `bdinfo` was read from the pinned `bl33.bin`'s machine code (`tools/package/bdinfo_audit.py`, run by
  `diag_memory_test`). Its function loads fields of the global data and board info, calls a print helper and printf,
  and stores only to its own stack frame. It uses no system instruction and does not touch the network or storage.
- `fdt addr` is used with the address alone. With a size it rewrites the blob in place (`fdt_open_into`), so a test
  rejects that form. It sets the `fdtaddr` variable in RAM; `saveenv` is not in the image's scripts.
- `fdt rsvmem` is only called as `fdt rsvmem print`; without a subcommand upstream U-Boot reads past `argv`.
- `fdt` behaviour is from upstream U-Boot v2018.01 and v2024.01 sources, not from this fork's. The fork's
  `bdinfo` field set matches the older layout.

## What it confirms

- The DRAM banks U-Boot believes it has (start and size), and where U-Boot put itself (`relocaddr`, `sp start`,
  `irq_sp`, `TLB addr`). The loader range `0xA0000000..0xA001EF00`, `bl33.bin` at `0xAA000000`, the DTB window at
  `0x8D000000` and the boot-args page can be compared with them by eye.
- That the control FDT has no reservations and what its `/memory` says.

## Limits

- Anything about BL31's or hekate's reservations, carveouts held by the memory controller, or memory that U-Boot does
  not track. A bank list with room at `0xA0000000` does not show the range is free.
- That the DTB copy or the loader fits the live memory map: nothing is copied.
- Whether the numbers equal those the loader sees after `bootm go`.

`fdt print`, `bdinfo` and `reset` have not run on a console yet; the first screen decides whether they behave as read.
