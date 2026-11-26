# T2 result: first hardware run

Run 2026-11-26 on the Erista, hekate 6.5.5, from Nyx "More configs", entry `iOS Diag T2`. One run, no retry. The user reported it from the photograph of the
loader screen and from the Mac's checks; the photograph is not stored. The record is `boot/diag/erista-t2-run1.txt`. Verdict: PASS.

The run was the owner's. The separate written approval for the one sensor write that `ERISTA_T2_RUN.md` section 3 asks for is not in this repository.
That write and the divisor write are done; no other sensor write, no retry and no further T2 run is approved.

| Item | Result |
| --- | --- |
| Screen | `T2 COMPLETE CODE 0000. COMPLETE`, `WRITES 160. TRANSFERS 35. NO RETRY. NO CAR WRITE. ONE SENSOR WRITE AT MOST` |
| Return | PSCI `SYSTEM_RESET` returned to hekate after `RESET IN 15 S`; no POWER hold |
| Nyx | start 34.5 C, end 35.6 C, +1.1 C across hekate, Nyx and the run. The peak during the run and the selection-to-return time were not recorded |
| Sensor | `0x4C`, ID `55`, configuration `C0`, status `00`, offsets `00.00` |
| One-shot | written; BUSY seen at 2005 us; BUSY clear at 31004 us; last status `00` |
| After | configuration `C0`, `BACK IN SHUTDOWN` |
| Mac | 12 of 12 tests, T2 build, `AUDIT PASSED (T2)` with the 13 addresses, the earlier loaders' hashes unchanged, 4 of 4 card hashes equal; `loader.uboot.bin` `ce46a5d2...cba` (full value in the record) |

## Readings

| | Remote | Local |
| --- | --- | --- |
| Before the one-shot (raw) | `22.10` = 34.0625 C | `1F.D0` = 31.8125 C |
| After (raw) | `22.50` | `1F.F0` |
| After (screen) | 34.3125 C | 31.9375 C |

Decode: sixteenths = high x 16 + (low >> 4). The two before figures are derived here from the raw words; the screen printed only the after pair. The before pair is
whatever the sensor latched earlier, and its age is unknown. OPEN was clear and neither high byte is at the range edge.

## What it shows

- **The write count matches the host model.** 160 writes and 35 transfers are `142 + 9n` and `2(15 + n) + 1` with n = 2 status polls, as `t2_bus_test` derives
  and `t2_measured_test` replays with these sensor bytes. A conversion that ends before the 31 ms poll gives n = 2; one that ends later gives 169 writes.
- **The one-shot form worked.** The write-byte form (pointer `0F`, value `00`), which was an inference in `ERISTA_T2_REVIEW.md` finding 3, was acknowledged and a
  conversion ran: BUSY was seen at the first poll and both temperature pairs changed.
- **BUSY is set during a one-shot** on this part, from one observation.
- **The sensor stays in shutdown.** The screen read `C0` before and after. Nyx cannot confirm it later, because hekate's `tmp451_init` rewrites the configuration.
- **The state words are stable.** Every CAR and I2C1 word equals T1 v3's (and T1-T's); the two CAR words equal D0-S v2's. The divisor stayed `00FF0001`, not
  restored, and hekate still worked after the reset.
- **PSCI `SYSTEM_RESET` from the loader at EL1 returned to hekate again**: D0, D0-R, T1-T, T1 v3, D0-S v2, D1-min and T2.

## What it does not show

- 31004 us is the time of the poll that saw BUSY clear and is not the conversion time. No completion is accepted before 31 ms, so the conversion may have ended
  earlier. 2005 us is the first poll.
- The run does not show temperature monitoring while XNU runs. It is one conversion at one moment in the loader.
- It does not show fan control. The fan was not touched.
- It does not give an autonomous hang protection. No watchdog or deadline exists in this image; the person with POWER is the stop.
- Nothing about a thermal limit. The 40, 50 and 55 C figures stay provisional and are not official limits.
- Whether the same write works under XNU, with another driver on the bus, or after hekate's own I2C use.
- Whether the console ran on USB power; the photograph shows the cable.

`boot` stays 0 and the feature mask stays 0. T3, T4, the full D1, any retry of T2, any other TMP451 write and every XNU run stay on HOLD; the T3/T4 waiver is
undecided. The Mac's blocker list is `ERISTA_FIRST_XNU_BLOCKERS.md`.
