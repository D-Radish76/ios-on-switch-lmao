// SPDX-License-Identifier: MIT
// raw::Device over the kernel model with the kernel's compression on, for raw_private_comp.cpp's tests: the compressed-memory
// thread makes what the device asks for, and the copy engine's launches are read back from the jobs submitted.
#pragma once
#include "raw_device_harness.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace private_comp {

constexpr uint32_t kBGRA8 = 80, kW = 1792, kH = 1472;

// The copy engine's source and destination of each launch the jobs since `first` submitted
inline std::vector<std::pair<uint64_t, uint64_t>> copies(size_t first)
{
    std::vector<std::pair<uint64_t, uint64_t>> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++)
        for (const TegraGM20BAbiPush &push : jobs[j].pushes) {
            const auto *w = reinterpret_cast<const uint32_t *>(kernel_model::model.host(push.va));
            uint32_t in[2] = {}, outs[2] = {};
            for (uint32_t i = 0; w && i < push.dwords;) {
                const uint32_t hdr = w[i++], op = hdr >> 29, subc = hdr >> 13 & 7, m = (hdr & 0xfff) << 2,
                               n = hdr >> 16 & 0x1fff;
                if (!hdr)
                    continue;
                if (op == 7)
                    break;
                if (op == 4) {
                    if (subc == 4 && m == NVB0B5_LAUNCH_DMA)
                        out.push_back({uint64_t(in[0]) << 32 | in[1], uint64_t(outs[0]) << 32 | outs[1]});
                    continue;
                }
                for (uint32_t k = 0; k < n && i < push.dwords; k++, i++) {
                    const uint32_t method = m + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0);
                    if (subc != 4)
                        continue;
                    if (method == NVB0B5_OFFSET_IN_UPPER)
                        in[0] = w[i];
                    else if (method == NVB0B5_OFFSET_IN_LOWER)
                        in[1] = w[i];
                    else if (method == NVB0B5_OFFSET_OUT_UPPER)
                        outs[0] = w[i];
                    else if (method == NVB0B5_OFFSET_OUT_LOWER)
                        outs[1] = w[i];
                    else if (method == NVB0B5_LAUNCH_DMA)
                        out.push_back({uint64_t(in[0]) << 32 | in[1], uint64_t(outs[0]) << 32 | outs[1]});
                }
            }
        }
    return out;
}

class DevicePrivateComp : public harness::DeviceTest {
protected:
    std::mutex kindsLock;
    std::vector<uint32_t> kinds;  // each VmBind's kind, the compressed-memory thread's among them

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.infoFlags = TegraGM20BInfoCompression;
        kernel_model::model.bound = [this](const TegraGM20BAbiBindOp &op) {
            std::lock_guard<std::mutex> l(kindsLock);
            kinds.push_back(op.kind);
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    bool bound(uint32_t kind)
    {
        std::lock_guard<std::mutex> l(kindsLock);
        return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    }

    // The test's wait for the compressed-memory thread to answer the device's first ask
    void awaitMade()
    {
        for (int i = 0; i < 5000 && !raw::DeviceHarness::compReady(dev); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ASSERT_TRUE(raw::DeviceHarness::compReady(dev)) << "no compressed memory was made";
    }

    // `make` once (plain: none of its size made yet, and asked for), then again on what was made
    // (`colour`: one the colour move takes, plainFromCompressed; a zeta moves into its m2v twin's image instead)
    template <typename Make>
    void compressedSecond(raw::Texture &first, raw::Texture &second, Make make, bool colour = true)
    {
        ASSERT_TRUE(make(first));
        EXPECT_FALSE(first.comptags);
        awaitMade();
        ASSERT_TRUE(make(second));
        EXPECT_TRUE(second.comptags);
        EXPECT_EQ(dev.privateCompressed(second), colour);
    }

    // As RawMetalDevice.mm makes a private RenderTarget|ShaderRead 2D texture: compressed where memory is ready, else
    // plain and wanting it
    void zoomTarget(raw::Texture &t)
    {
        if (dev.createCompressedTexture(kW, kH, kBGRA8, raw::kCompKindC32, t))
            return;
        ASSERT_TRUE(dev.createTexture(kW, kH, kBGRA8, t, false, true));
        ASSERT_TRUE(dev.wantCompressed(t, raw::kCompKindC32, &t));
    }

    void commitEmpty()
    {
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }
};

}  // namespace private_comp
