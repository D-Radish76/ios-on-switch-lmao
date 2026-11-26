# Next diagnostics: T2 open items, a smaller D1, framebuffer and SD engine reads

Design only. No further hardware run is approved: T3, T4, D1, T1-H and any T2 repeat stay on hold. T2 itself ran once (`ERISTA_T2_RESULT.md`).

## T2: what is still open

From `ERISTA_T2_DESIGN.md`, "Not decided", by what would settle each. The first run settled the two hardware rows below for one observation each.

| Open item | Settled by | Hardware needed |
| --- | --- | --- |
| Send-byte or write-byte for register `0F` | the TMP451 data sheet's timing diagram | no |
| Read addresses of the remote offset registers (hekate uses `11` and `12` both ways) | the data sheet's pointer list | no |
| Whether BUSY is set during a one-shot, and whether 31 to 34 ms is the whole interval | BUSY was seen at the first poll and clear at the 31 ms poll. The interval is still unknown: no completion is accepted before 31 ms | settled for BUSY, open for the interval |
| Whether the sensor ACKs reads other than the ID at the changed divisor, in shutdown | T2: all 35 transfers (17 register reads and the one-shot) ended without a stop code at `00FF0001` | settled |
| Who may run T2 again, and at what bus rate | a decision. The divisor `00FF0001` is the only rate that has run | no |
| Polling in the loader versus ALERT or a timer in a kernel driver | a decision for the kernel driver. The loader polls by counter like every loader stage | no |

The first two needed only the data sheet. The sensor write count stays at one.

## D1: a smaller first step

D1-pre (U-Boot only, `iOS Diag SD`, `iOS Diag SD Info`) has run. The implemented D1 (`boot/diag/sd_loader`) loads the
pinned exporter, calls two U-Boot helpers and then `go`es into the exporter. The first run of any code that is not the
diagnostic's own image is the risky part, so D1 can be cut into steps, each run only after the one before it is reviewed:

| Step | Adds | Executes outside the diagnostic's image | Shows |
| --- | --- | --- | --- |
| D1-min | the D0 image plus `mmc dev 1` and `mmc read` of sector 0 to `0xA001F200`; U-Boot's script prints the CRC32 of those 512 bytes and the result of the `0x55AA` signature test; the loader prints only D0-R's report | no | the card read by U-Boot lands where the exporter's record will later live, and that range holds |
| D1-b | `bootm loados` of the exporter to `0xA7000000`; the loader checks its data CRC32 there | no | the exporter sits intact in RAM |
| D1-c | the two helper calls (`go 0xAA001920`, `go 0xAA001A64`) | yes (U-Boot's own code) | each returns 1 |
| D1 | `go 0xA7000000` and the record validation | yes (the exporter) | the record |

D1-min and D1-b write nothing to the card and touch no MMIO outside the diagnostic's allowlist. The sector's contents are
never printed (a disk signature identifies the card); U-Boot's script prints only the CRC and the signature result. Whether the script can
express D1-min and D1-b by dropping steps from the D1 template is unchecked.

## Framebuffer attributes and SD engine state

Both can be shown by one read-only diagnostic image, with no new class of risk.

**Framebuffer.** The D0 and D0-R scripts run the same `/framebuffer` check as the normal script
(`boot/diag/loader/u-boot-script.txt:30-34`: address `0xF5A00000`, size field 4, 720x1280, `a8b8g8r8`, rot 3) and only
reach the loader when it passes. Both runs reached the loader, so the check passed on the Erista. That is an inference
from the control flow: the values were not printed. Printing them needs `echoe` lines of the environment variables in
the script, no MMIO.

**SD engine state.** `loader.c:485-491` refuses `0x710` if SDMMC1's present-state word (`0x700B0024`) has bits `0x307`
set or the software-reset byte (`0x700B002F`) is non-zero. D0 and D0-R run after `bootm prep`, the same moment, so two
`switch_diag_read32` loads (`0x700B0024` and the word at `0x700B002C`, byte 3) would show the same state the normal
loader would see. The rule "no load or store to `0x700B0000`" in the D1 design would be relaxed to those two loads, and
`diag_loader_inspect.py`'s allowlist and `diag_mmio_audit.py` would gain them.

A combined image (call it D0-S) needs a new BUILD target, a new image hash and review of the allowlist change. It does not
run until approved.
