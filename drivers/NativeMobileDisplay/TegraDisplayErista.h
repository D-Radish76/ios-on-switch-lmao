// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2018 naehrwert
// Copyright (c) 2018-2025 CTCaer
// The T210 (Erista, HAC-001 LCD) side of hekate's display_init (bdk/display/di.c) and its register tables (bdk/display/di.inl):
// what differs from the T210B01 tables in TegraDisplayPower.cpp. DSI and MIPI_CAL indices are 32-bit word indices.
#pragma once
#include <stdint.h>

namespace tegra_display_erista {

struct Reg {
    uint32_t index, value;
};

constexpr uint32_t kHidrevMajorT210 = 1;
constexpr uint32_t kHidrevMajorT210B01 = 2;
constexpr uint32_t kHidrevOffset = 0x804;

constexpr uint32_t kDsiInitSeqData15 = 0x5F;
constexpr uint32_t kBtaTiming = 0x50204;
constexpr uint32_t kMipiBiasPadCfg1 = 0x300;
constexpr uint32_t kPlldMisc1 = 0x20;
constexpr uint32_t kPlldMiscLowPower = (1u << 21) | (1u << 19) | (1u << 18) | (1u << 16) | 0x0AAA;

// APB_MISC pinmux AUX registers (bytes), which hekate pulls down for the LCD driver (NFC_EN, NFC_INT) and the WLED driver.
constexpr uint32_t kPinmuxNfcEn = 0x3000 + 0x1D0, kPinmuxNfcInt = 0x3000 + 0x1D4;
constexpr uint32_t kPinmuxLcdBlPwm = 0x3000 + 0x1FC, kPinmuxLcdBlEn = 0x3000 + 0x200;
constexpr uint32_t kPinmuxPwm0Function = 1, kPinmuxTristate = 1u << 4;
constexpr uint32_t kPinmuxPulledDown[] = {kPinmuxNfcEn, kPinmuxNfcInt, kPinmuxLcdBlPwm, kPinmuxLcdBlEn};

constexpr uint32_t kPanelJdiXxx062m = 0x10, kPanelInlP062ccaAz1 = 0x0F20, kPanelAuoA062tan01 = 0x0F30;

constexpr uint32_t kDsiWrData = 0x0A, kDsiTrigger = 0x13, kTriggerHost = 1u << 1;

// _di_dsi_pad_cal_config_t210: DSI_PAD_CONTROL_1..4, pre-emphasis 3 on every lane.
constexpr Reg kDsiPadCal[] = {
    {0x4F, 0}, {0x50, 0}, {0x51, 0x3333}, {0x52, 0},
};
// _di_mipi_dsi_cal_prod_config_t210, then _di_mipi_dsi_cal_unused_config.
constexpr Reg kMipiDsiCal[] = {
    {0x38 / 4, 0x200200}, {0x3C / 4, 0x200200}, {0x64 / 4, 0x200002}, {0x68 / 4, 0x200002}, {0x14 / 4, 0},
    {0x18 / 4, 0},        {0x1C / 4, 0},        {0x20 / 4, 0},        {0x24 / 4, 0},        {0x28 / 4, 0},
    {0x40 / 4, 0},        {0x44 / 4, 0},        {0x68 / 4, 0},        {0x70 / 4, 0},        {0x74 / 4, 0},
};
// _di_dsi_panel_init_config_jdi.
constexpr Reg kPanelInitJdi[] = {
    {kDsiWrData, 0x0439},
    {kDsiWrData, 0x9483FFB9},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0xBD15},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0x1939},
    {kDsiWrData, 0xAAAAAAD8},
    {kDsiWrData, 0xAAAAAAEB},
    {kDsiWrData, 0xAAEBAAAA},
    {kDsiWrData, 0xAAAAAAAA},
    {kDsiWrData, 0xAAAAAAEB},
    {kDsiWrData, 0xAAEBAAAA},
    {kDsiWrData, 0xAA},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0x01BD15},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0x2739},
    {kDsiWrData, 0xFFFFFFD8},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFF},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0x02BD15},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0xF39},
    {kDsiWrData, 0xFFFFFFD8},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFFFF},
    {kDsiWrData, 0xFFFFFF},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0x00BD15},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0x06D915},
    {kDsiTrigger, kTriggerHost},
    {kDsiWrData, 0x439},
    {kDsiWrData, 0x000000B9},
    {kDsiTrigger, kTriggerHost},
};

// display_init's SET_POWER_CONTROL (0xB1) payload for the INL P062CCA-AZ1 and the AUO A062TAN01, and its shared tail.
constexpr uint32_t kPowerControlInl = 0x751548B1, kPowerControlAuo = 0x711148B1, kPowerControlTail = 0x143209;

}  // namespace tegra_display_erista
