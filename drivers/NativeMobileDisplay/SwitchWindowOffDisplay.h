// SPDX-License-Identifier: BSD-2-Clause-Patent
void requestWindowsOff(uint32_t windows)
{
    const uint32_t bases[] = {kWinABase, kWinBBase, kWinCBase};
    const uint32_t updates[] = {kWinAUpdate, kWinBUpdate, kWinCUpdate};
    const uint32_t requests[] = {kWinAActReq, kWinBActReq, kWinCActReq};
    uint32_t update = kGeneralUpdate, request = kGeneralActReq;
    for (unsigned i = 0; i != 3; ++i) {
        if (!(windows & requests[i]))
            continue;
        dc[bases[i] + native_planes::kOptions] = 0;
        update |= updates[i];
        request |= requests[i];
    }
    scanoutMemoryBarrier();
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
}

bool windowsOffTaken(uint32_t windows)
{
    const uint32_t bases[] = {kWinABase, kWinBBase, kWinCBase};
    const uint32_t requests[] = {kWinAActReq, kWinBActReq, kWinCActReq};
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    bool disabled = true;
    for (unsigned i = 0; i != 3; ++i)
        if ((windows & requests[i]) && (dc[bases[i] + native_planes::kOptions] & native_planes::kWinEnable))
            disabled = false;
    dc[kDcStateAccess] = access;
    return disabled && !(dc[kDcStateControl] & windows);
}
