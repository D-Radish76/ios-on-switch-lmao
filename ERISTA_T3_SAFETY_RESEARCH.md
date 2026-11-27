# T3 safety research: fan, A5, CC4 and POWER hold

Research and documents only, 2026-11-28. No code changed, nothing run, nothing copied to a card. It does not approve a run.

## How to read it

| Mark | Meaning |
| --- | --- |
| P | Primary: source code or a vendor statement, read at the version named |
| S | Secondary: community wiki, forum post, retailer page |
| I | Inference from P or S, stated as such |
| M | Measured by this project (owner's reports) |
| U | Not found, or not reachable from here |

Pages were read through a fetch tool that summarises. Anything marked "tool summary" is a paraphrase, not a verbatim quote. Line numbers were not available; functions and files are named instead. Check a P item against the tagged file before relying on it.

## What was not reachable

- The Tegra X1 (T210) TRM and the MAX77620 datasheet: no copy was fetched. Search found only unrelated Maxim parts.
- A Nintendo or board-level schematic of HAC-001: none found. The repair wikis describe symptoms, not nets.
- Switchroot's Linux device trees (`l4t-device-tree-overlays`, GitLab) and perillamint's `tegra210-nintendo-switch.dts`: the pages returned metadata only or are blocked to fetchers. Whatever Linux knows about the fan and the 5 V rail is **U** here.
- A datasheet for the fan: none exists publicly (the retailer says so).
- The MAX8969 datasheet (named by a forum poster): not fetched.

## Findings

### A5 and CC4, from hekate v6.5.5

Source: `https://raw.githubusercontent.com/CTCaer/hekate/v6.5.5/bdk/power/regulator_5v.c` and `regulator_5v.h`, `bdk/thermal/fan.c`, `bdk/soc/hw_init.c`.

| # | Finding | Mark |
| --- | --- | --- |
| 1 | `regulator_5v_enable`, when no device holds the rail yet: set `PINMUX_AUX_SATA_LED_ACTIVE` to 1, drive GPIO A5 high ("power the fan and rail from the battery 5 V regulator"); on T210 only, set `PINMUX_AUX_USB_VBUS_EN0` to `PINMUX_LPDR \| 1` and drive CC4 low; clear `PMC_NO_IOPOWER_GPIO`; clear `PMC_PWR_DET_33V_GPIO`. A source comment says the supply selection between battery and USB is automatic (tool summary) | P |
| 2 | `regulator_5v_disable`: clear the device bit; with no device left, drive A5 low, and on T210 drive CC4 low | P (tool summary) |
| 3 | `regulator_5v_usb_src_enable(bool)`, T210 only: enable drives CC4 **high**, disable drives it low. So CC4 high is the USB VBUS source state and CC4 low is the state hekate uses while the fan is on | P (tool summary) |
| 4 | Device bits: `REGULATOR_5V_FAN` bit 0, `REGULATOR_5V_JC_R` bit 1, `REGULATOR_5V_JC_L` bit 2, `REGULATOR_5V_ALL` 0xFF. hekate counts three users on the A5 rail: the fan and both Joy-Con | P |
| 5 | `fan_set_duty` enables the rail first (`regulator_5v_enable(REGULATOR_5V_FAN)`), then writes `PWM_CSR_EN \| (inv_duty << 16)` to `PWM_CSR_1`, then sets `LCD_GPIO2` to 1. Disable writes `PWM_CSR_EN \| (0x100 << 16)`, calls `regulator_5v_disable`, and writes `LCD_GPIO2` as input, parked, tristate, pull down (`0x74`) | P |
| 6 | `fan_set_from_temp`: 52 C and up duty 102, 47 C and up duty 76, 42 C and up duty 51, 39 C and below duty 0, nothing between 39 C and 42 C. Duty 51 is hekate's own lowest running duty | P |
| 7 | `fan_set_duty` init block: tach pad `CAM1_PWDN` gets `PINMUX_TRISTATE \| PINMUX_INPUT_ENABLE \| 1` plus a pull-up on T210 only; GPIO S7 input; `PWM_CSR_1` set to the off word; `LCD_GPIO2` to 1; GPIO V4 to SPIO ("Fan power mode") | P |
| 8 | `hw_deinit`, under `BDK_HW_EXTRA_DEINIT` only: `fan_set_duty(0)` and `regulator_5v_disable(REGULATOR_5V_ALL)` before DRAM and MMU teardown. Whether that macro is set in the v6.5.5 payload build is **not established** here | P, scope U |

Findings 1 to 3 come from one tool summary of `regulator_5v.c` and should be read against the file. The T3 write list matches findings 5 and 7 word for word (`0x80B90000` is `PWM_CSR_EN \| (185 << 16)`, `0x81000000` the off word, `0x00000074` the parked pad).

### What the rail feeds

| # | Finding | Mark |
| --- | --- | --- |
| 9 | Switchbrew Testpads, cluster H pad 10: "Power for JoyCon charge circuit and fan", level 5V. `https://switchbrew.org/wiki/Testpads` | S |
| 10 | repair.wiki: "The Nintendo Switch shares the same power rail for the fan as it does for the joycons." The article names a MOSFET just north of M92T36 and gives no voltage or regulator. `https://repair.wiki/w/Nintendo_Switch_No_Fan_Spin_Voltage_Injection_Method` | S |
| 11 | tronicsfixforum thread 9986: poster sublime167 states a MAX8969 provides 5 V to the fan and Joy-Con and says the distribution through a dual MOSFET is presumed, not traced. Poster arar measured: no 5 V with a 15 V charger, 5 V only with a 5 V charger once the console is on. `https://www.tronicsfixforum.com/t/switch-no-fan-joycon-charge-when-docked-or-connected-to-a-15v-charger/9986` | S, partly measured by the posters |
| 12 | Switchbrew Hardware: Erista has PMIC MAX77620AEWJ and sub-PMIC MAX77621AEWI (Mariko and later use MAX77812). `https://switchbrew.org/wiki/Hardware` | S |
| 13 | The rail serves the fan, the two Joy-Con rails and, through hekate's `usb_src` path, is tied to the USB VBUS source choice. The exact topology (regulator, MOSFETs, what CC4 gates) is not in any source read | I |

### The fan

| # | Finding | Mark |
| --- | --- | --- |
| 14 | A replacement fan sold as "Nintendo Switch Cooling Fan" (SparkFun SPX-18592, also Core Electronics): "Pulls up to 0.4A at 5VDC", 8000 RPM maximum, 25 kHz PWM, four wire FPC at 0.5 mm pitch, pinout 5V, Tach, GND, PWM. No datasheet; Core Electronics says it appears to be a reproduction of the OEM part, and neither page names the console model. `https://www.sparkfun.com/nintendo-switch-cooling-fan.html`, `https://core-electronics.com.au/nintendo-switch-cooling-fan.html` | S, and it describes a reproduction part |
| 15 | iFixit's fan replacement guide gives no electrical rating. A Reddit thread asking for the maximum current has no figure | U |
| 16 | The OEM fan's rating for HAC-001 is not known. 0.4 A at 5 V is an upper figure for a similar part at full speed. T3 runs duty 51 of 255 | I |

### POWER hold

| # | Finding | Mark |
| --- | --- | --- |
| 17 | Nintendo support: normal shutdown is a three second hold and the menu; force shutdown is "press and hold down the POWER Button for at least twelve seconds" when the system does not respond. `https://en-americas-support.nintendo.com/app/answers/detail/a_id/22727`. The page says nothing about the mechanism | P (vendor instruction) |
| 18 | NVIDIA developer forum, 2017, a reply from a user named Trumany: "The time delay for power button is 2s ~ 12s, which can be set with register ONOFFCNFG1 of MAX77620", to be written after each power on. The poster's standing is not known. `https://forums.developer.nvidia.com/t/how-can-i-change-the-power-down-time-of-max77620/52396` | S |
| 19 | Linux `include/linux/mfd/max77620.h`: `MAX77620_ONOFFCNFG1_MRT_MASK 0x38`, `MRT_SHIFT 0x3`, `SFT_RST BIT(7)`. hekate's `max77620.h` has the same names. Neither header comments on what MRT means | P (names only) |
| 20 | Switchbrew Testpads lists the power button as a pad ("Power Button", clusters E and J) next to pads marked "From PMIC" | S |
| 21 | Reading 17 to 20 together, the 12 second force off is probably the MAX77620's manual reset time, driven by the power button line and running inside the PMIC with no help from the SoC. The 12 s figure equals the top of the range in 18. Nothing read states this, no datasheet was reachable, and whether it removes power from the A5 regulator is also unstated | I |
| 22 | This project's own record: the D0-S v1 freeze was recovered by hand, and RCM entry, payload injection and manual recovery succeeded before (owner's report, 2026-11-27 14:47 JST). Those freezes happened with the rail off. The recovery time from a hang with the rail on is unmeasured and the rehearsal was skipped | M, scope limited |

### SYSTEM_RESET and the A5 and CC4 pins

| # | Finding | Mark |
| --- | --- | --- |
| 23 | In the T3-R run the reads before any write gave A5 `CNF` 1, `OE` 1, `OUT` 0 and CC4 `CNF` 1, `OE` 1, `OUT` 0: A5 and CC4 were outputs driven low when our image started. Source: `ERISTA_T3R_RESULT.md` | M |
| 24 | After a `SYSTEM_RESET` the A5 and CC4 pins' state is not known. Nothing was read through that path | U |
| 25 | hekate re-initialises the fan on its own: `fan_set_from_temp` with a temperature at or below 39 C reaches `fan_set_duty(0)`, which calls `regulator_5v_disable` and drives A5 low (findings 2 and 6), and Nyx calls it every 5 s. So an A5 left high across a reset would be driven low by hekate once the SoC is at or below 39 C. Between 40 C and 41 C it does nothing, and above 42 C hekate keeps the rail on itself | I |

Update, 2026-11-28 02:05 JST: the owner rehearsed a POWER hold in normal hekate at 24 C. The screen went dark after about 3 to 4 s and the RCM return worked. That is evidence for finding 22 in normal hekate only. It does not show a full power cut, and it does not cover a hang or a rail that is on. Details in `ERISTA_T3_RUN.md`.

## What this does to each remaining risk

| Risk | Before | After | Status |
| --- | --- | --- | --- |
| CC4 polarity | Role unknown | hekate uses CC4 as the USB VBUS source enable: high selects the USB source (3), low is its state while the fan runs (1, 3). T3 never writes CC4 and requires it low, and the T3-R run read it low as an output (23). No schematic or datasheet confirms what the pin gates | Partly resolved at source level; hardware polarity unconfirmed |
| Rail loads | Unknown | The users are the fan and the two Joy-Con rails (4, 9, 10). With both Joy-Con detached the load is the fan. Whether anything else shares the rail (USB VBUS source path) is not shown (13) | Partly resolved |
| Fan current | Unknown | An upper figure of 0.4 A at 5 V for a similar replacement part (14), not the OEM rating (16). hekate runs the same fan at duty 51 and above whenever the SoC is at 42 C or more, with Joy-Con attached (6) | Bounded, not measured |
| A5 high with CC4 low | Not judged | Exactly hekate's enable sequence on T210 (1, 5). T3's words are hekate's words (5, 7) | Resolved at source level |
| Hang with the rail on | Open | Unchanged in kind: software limits cannot run. The effect of a hang is a fan at duty 51 and A5 high, the state hekate holds indefinitely when hot (6). The PWM block runs without the CPU (I, from the register design, not checked here). No source says what a hang does to the SoC's temperature, and the loader has no thermal check | Consequence bounded by analogy; hang itself not removable |
| POWER hold recovery | Unmeasured | Vendor says 12 s or more for an unresponsive console (17); the mechanism is probably in the PMIC (21), which would make it independent of a SoC hang, but this is inference. Not measured with the rail on | Partly supported, not verified |
| A5 and CC4 across `SYSTEM_RESET` | Unknown | Still unknown (24). hekate's own temperature path would drive A5 low once cool (25) | Unchanged, with a mitigating inference |
| Latch read is not a voltage | Stated | Unchanged. No source gives a way to read the 5 V rail from the SoC | Unchanged |
| Readback exactness of `PWM_CSR_1` and the pad word | Untested | Unchanged. hekate writes the same words but does not read them back | Unchanged |

Nothing here reduces the hang risk to zero, and no figure in this document is measured on this console except 22 and 23. The open items that would settle the rest need a schematic or datasheet, or a bench measurement by the owner:

- the net that CC4 gates and the source of the A5 rail (a schematic, or a continuity and voltage check by the owner, not by Claude);
- the OEM fan's current at 5 V (an owner's meter reading, or a supplier datasheet);
- the MAX77620 manual reset time and what it does to the A5 regulator (the datasheet);
- the A5 and CC4 pins after `SYSTEM_RESET` (only a run can show it, and a run is not approved).

## Reading for the decision

The research supports one change in how the risks are stated: A5 high with CC4 low is hekate's own fan enable, the fan load at duty 51 is below what hekate puts on the same rail whenever the SoC is warm, and CC4 is the USB source switch that T3 does not touch. It does not support calling the hang risk accepted. A hang with the rail on leaves the fan running until the power is cut, the 12 second hold is the only exit, and it has not been tried in that state. Approval remains with the owner; see `ERISTA_T3_RUN.md`.
