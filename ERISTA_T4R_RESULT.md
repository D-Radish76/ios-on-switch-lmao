# T4-R: first run, result

Run on 2026-11-27 by the owner, one time, no retry, no manual stop. The record is `boot/diag/erista-t4r-run1.txt` (`result=READ_AND_RETURN_OK`), taken from the photograph IMG_8746.jpg (read first by ChatGPT, checked against the photograph here). The photograph was taken while `RESET IN 15 S` was on the screen. It is not stored.

Setting: Erista HAC-001, hekate 6.5.5, Auto Boot off, the menu showed `iOS Diag T4R` only, case temperature 29.5 C before and 31.3 C after (+1.8 C). The temperature during the run was not measured.

## What the screen showed

| Read | Address | Word |
| --- | --- | --- |
| `TMRUS` 1 | `0x60005010` | `056E3BEE` |
| `TMR5_PTV` | `0x60005060` | `00000000` |
| WDT0 config | `0x60005100` | `00000000` |
| WDT0 status | `0x60005104` | `00000000` |
| `TMRUS` 2 | `0x60005010` | `057DA780` |

State line `T4R WDT0 AFTER HEKATE INIT: IDLE`; count 0, expiry 0; `T4R COMPLETE. READS 5. STOP 0000`; then `RESET IN 15 S`. Hekate came back by itself. The board lines were `ERISTA T210B01=0 SKU=0 BOOT=0`, `EL=4`, `CNTFRQ=19200000`, `CRC32 1 OK`.

Checks made here: `0x057DA780 - 0x056E3BEE = 1010578`, the figure the screen printed. About ten shown steps of 100 ms lie between the two `TMRUS` reads, so a gap of 1.01 s agrees with `TMRUS` counting in microseconds. The replay of the five words through the real C logic gives these 14 screen lines exactly and the state IDLE (`diag_t4r_measured_test`).

## Two kinds of success

Read and return: met (`READ_AND_RETURN_OK`). All five reads returned, `TMRUS` advanced, the state and the count line were drawn, `COMPLETE` said 5 reads and stop 0000, `RESET IN 15 S` was drawn and hekate came back by itself. These are the pass conditions of `ERISTA_T4R_RUN.md` for the screen and the return.

Formal pass: not claimed. The run sheet also asks for conditions this report does not hold:

| Condition | Held |
| --- | --- |
| cold start | not recorded |
| temperature written down, under 40 C | yes, 29.5 C |
| video on | not recorded |
| a hand on POWER for the whole run | not recorded |
| card holds only this entry | yes, the menu showed one entry |
| COMPLETE to hekate timed | an impression of about 15 s, not measured; it does not fit the exit path (see the timing section of the run sheet) |
| card hash record after the run | not done |
| inis moved back from `_ini_stash` | not done |

The run is not turned into a formal pass by this record, and no stop condition or time figure of the run sheet is changed by it.

## What the run shows

- The four words are readable at EL1 here, through the gate, with no hang and no exception. That answers one open item for reads only: nothing is known about writes.
- After hekate's init the timer 5 value, the WDT0 config and the WDT0 status are all zero, which the classification calls IDLE. Nothing was running on WDT0 or timer 5 at that moment.
- `TMRUS` advanced as a microsecond counter would.
- The loader's timed PSCI `SYSTEM_RESET` returned the console to hekate once more.

## What it does not show

That any write to these registers works; that a watchdog can be armed, expires or resets the console; what bits 0 and 1 of the status mean (a zero word does not name them); that WDT0 on timer 5 is the watchdog the DTB calls index 7; the state right after a reset; where the console lands after a watchdog reset (the PSCI path is not that path); the reset time of the PSCI call; anything about XNU. The run does not make T4-A safe or approved.

## Limits that stay

T4-R is not approved to run again. T4-A and T4-P are not implemented, no watchdog is armed, no new MMIO write exists, `boot` 0, feature mask 0, T3 and T4 are not waived, the first XNU run is on HOLD, and the ten diagnostic hashes are unchanged. T4-A's design is on paper in `ERISTA_T4A_DESIGN.md`.
