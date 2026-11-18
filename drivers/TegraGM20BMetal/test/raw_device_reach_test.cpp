// SPDX-License-Identifier: MIT
// CommandBuffer::reach (raw_reach.h) on raw::Device over the kernel model. A pass whose descriptor names no size (Metal's
// renderTargetWidth and renderTargetHeight left 0), as the range encode hands on a loaded YCbCr target's pass, reaches over
// its attachments' area.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_reach.h"
#include "../raw/runtime/raw_record.h"
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kRGBA8 = 70;

// A pass with draws whose replay draws none: the reach target is cleared and reduced to its tiles
class UndrawnReplay final : public raw::DrawReplay {
public:
    bool any() const override
    {
        return true;
    }

    bool atFrameResolution() const override
    {
        return false;
    }

    bool replay(raw::RenderEncoder &, const char **) override
    {
        return true;
    }
};

class DeviceReach : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        DeviceTest::SetUp();
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=13 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = raw::Pack::fnv(raw::kReachReduceState, strlen(raw::kReachReduceState));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    }
};

TEST_F(DeviceReach, APassNamingNoSizeReachesOverItsAttachment)
{
    raw::Texture target;
    ASSERT_TRUE(dev.createTexture(64, 48, kRGBA8, target));
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::PassDesc p;
    p.color[0].texture = &target;
    p.color[0].load = raw::ColorAttachment::Load;
    p.color[0].store = true;
    p.count = 1;
    UndrawnReplay draws;
    raw::PassReach reach;
    const char *why = "";
    ASSERT_TRUE(cb->reach(p, p.width, p.height, draws, reach, &why)) << why;
    ASSERT_NE(reach.mask, nullptr);
    EXPECT_FALSE(reach.memory);
    EXPECT_EQ(reach.tilesX, 2u);
    EXPECT_EQ(reach.tilesY, 2u);
    EXPECT_EQ(reach.mask->width, 2u) << "a texel a 32x32 tile";
    EXPECT_EQ(reach.mask->height, 2u);
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
}

}  // namespace
