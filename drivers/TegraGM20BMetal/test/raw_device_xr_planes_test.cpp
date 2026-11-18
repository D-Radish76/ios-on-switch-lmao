// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 held in a surface's planes (createXrPlaneTexture): over two planes whose rows are off 32 bytes, its
// header pair reads the planes' pool images; over a plane alone, its alpha a memoryless target whose header, freed with the
// pair, is over a sparse range of its own (reads 0, stores dropped); turned, a pass into it draws its planes turned.
#include "raw_device_views_harness.h"
#include "../raw/runtime/raw_rotated_pass.h"
#include "../raw/runtime/raw_turned_planes.h"
#include "../raw/runtime/raw_xr_pass.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace views;

constexpr uint32_t kRGBA8 = 70;

// A header's address to 512 bytes, pitch (ADDRESS_BITS31TO5) or block-linear (fx_mip.cpp ADDRESS_BITS31TO9)
uint64_t headerAddress(const uint32_t *w)
{
    return uint64_t(w[1] & ~511u) | uint64_t(w[2] & 0xffff) << 32;
}

struct Surface {
    size_t bytes = 8 * size_t(getpagesize());
    void *base = aligned_alloc(size_t(getpagesize()), bytes);

    ~Surface()
    {
        free(base);
    }
};

TEST_F(DeviceViews, PlanesOfRowsOff32AreReadThroughTheirPools)
{
    Surface s;
    raw::Texture t;
    const char *why = "";
    // 64x64: words in rows of 272 bytes, alpha bytes in rows of 80 after them
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 272, 64 * 272, 80, 64, 64, 550, t, &why))
        << why;
    ASSERT_TRUE(t.xrPlanes && t.luma->sampled && t.chroma->sampled);
    for (uint32_t k = 0; k < 2; k++) {
        const raw::Texture &pool = *(k ? t.chroma : t.luma)->sampled;
        EXPECT_EQ(headerAddress(dev.headerWords(t.tic + k)), pool.va & ~uint64_t(511)) << "plane " << k;
    }
    dev.destroyTexture(t);
}

class DeviceXrSink : public DeviceViews {
protected:
    std::vector<TegraGM20BAbiBindOp> ops;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.infoFlags = TegraGM20BInfoSparse;
        kernel_model::model.bound = [this](const TegraGM20BAbiBindOp &op) {
            ops.push_back(op);
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    void TearDown() override
    {
        kernel_model::model.bound = nullptr;
    }
};

// An M4 Max reads a plane alone's alpha as 0 and stores none of it (m2v-textures9 probes/xr.txt 6a-6c): its header is over
// a sparse range of the texture's own, one VmBind MapSparse at creation, unbound with the texture
TEST_F(DeviceXrSink, APlaneAlonesAlphaIsASparseSinkFreedWithItsPair)
{
    Surface s;
    raw::Texture t, other, plain;
    const char *why = "";
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 0, 0, 64, 64, 550, t, &why)) << why;
    ASSERT_TRUE(t.xrPlanes && t.luma && t.chroma);
    EXPECT_TRUE(t.chroma->memoryless);
    EXPECT_EQ(t.chroma->tic, t.tic + 1);
    const uint64_t va = t.window.va, bytes = t.window.bytes;
    ASSERT_GE(bytes, 64u * 64u);
    const auto sparse = [&](uint32_t op) {
        size_t n = 0;
        for (const TegraGM20BAbiBindOp &o : ops)
            n += o.op == op && o.va == va && o.range == bytes;
        return n;
    };
    EXPECT_EQ(sparse(TegraGM20BBindMapSparse), 1u);
    const uint32_t *alpha = dev.headerWords(t.tic + 1);
    EXPECT_EQ(uint64_t(alpha[1] & ~31u) | uint64_t(alpha[2] & 0xffff) << 32, va) << "a pitch header over it";
    dev.destroyTexture(t);
    EXPECT_EQ(sparse(TegraGM20BBindUnmap), 1u) << "unbound with the texture";
    ASSERT_TRUE(dev.createTexture(16, 16, kRGBA8, plain));
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 0, 0, 64, 64, 550, other, &why)) << why;
    EXPECT_NE(plain.tic, other.tic);
    EXPECT_NE(plain.tic, other.tic + 1) << "the alpha's header went back with the pair alone";
    dev.destroyTexture(other);
    dev.destroyTexture(plain);
}

TEST_F(DeviceViews, APlaneAlonesAlphaNeedsSparseVa)
{
    Surface s;
    raw::Texture t;
    const char *why = "";
    EXPECT_FALSE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 0, 0, 64, 64, 550, t, &why));
    EXPECT_NE(std::string(why).find("TegraGM20BInfoSparse"), std::string::npos) << why;
}

TEST_F(DeviceViews, ATurnedTwoPlaneTargetDrawsItsPlanesTurned)
{
    Surface s;
    raw::Texture t;
    const char *why = "";
    // a plane of 64x32 (rows of 256 and 64 bytes), turned a quarter: a frame of 32x64
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 32 * 256, 64, 64, 32, 550, t, &why))
        << why;
    t.rotation = 1;
    raw::PassDesc p;
    p.color[0].texture = &t;
    p.color[0].load = raw::ColorAttachment::Clear;
    p.color[0].store = true;
    p.count = 1;
    ASSERT_EQ(raw::expandXrPlanes(p, dev), nullptr);
    raw::Texture targets[8];
    raw::turnXrPlaneTargets(t, p, targets);
    raw::Rotation r;
    ASSERT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::None);
    EXPECT_EQ(r.turn, 1);
    EXPECT_EQ(r.width, 32u);
    EXPECT_EQ(r.height, 64u);
    ASSERT_EQ(p.count, 2u);
    EXPECT_EQ(p.color[0].texture->va, t.luma->va) << "the words drawn straight into plane 0's rows";
    EXPECT_EQ(p.color[1].texture->format, raw::kXrAlphaFormat);
    dev.destroyTexture(t);
}

}  // namespace
