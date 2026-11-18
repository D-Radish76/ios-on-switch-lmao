// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#import "RawMetalMultisample.h"
#import "RawMetalPassCache.h"
#include "NativeBridgeMetalFrontend.h"

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d)
{
    return raw::shapes::storedSamples(textureShape(d));
}

static const char *zetaMemoryRefusal(int asked)
{
    switch (asked) {
    case -3:
        return "m2v made no texture";
    case -1:
        return "the frontend or NVK has no texture memory accessor";
    case -2:
        return "its texels are no one NVK image's";
    case 2:
        return "NVK's image has more than one plane";
    case 3:
        return "NVK's image is not bound";
    case 4:
        return "NVK's image has more than one level";
    case 5:
        return "NVK's image has more than one layer";
    case 6:
        return "NVK's image is multisampled";
    default:
        return "NVK names no image";
    }
}

static id storedZetaTwinExcluded(RMTexture *t, id<MTLDevice> m2v, const char **why)
{
    @synchronized(t) {
        if (t->_twin)
            return t->_twin;
    }
    if (tRawEncoding) {
        *why = "it was asked for inside a raw encode";
        return nil;
    }
    id twin = [m2v newTextureWithDescriptor:t->_descriptor];
    struct switch_nvk_image_memory m;
    const int asked = !twin                                 ? -3
                      : !native_bridge_metal_texture_memory ? -1
                                                            : native_bridge_metal_texture_memory(twin, &m);
    if (asked) {
        *why = zetaMemoryRefusal(asked);
        return nil;
    }
    raw::ZetaMemory z;
    z.va = m.addr;
    z.bytes = m.size_B;
    z.layerBytes = m.array_stride_B;
    z.pitch = m.row_stride_B;
    z.bpp = m.texel_B;
    z.blockHeight = m.y_log2;
    z.blockDepth = m.z_log2;
    z.tiled = m.tiled;
    z.compressed = m.compressed;
    @synchronized(t) {
        if (!gRM->raw->moveOwnedZeta(t->_raw, z, why))
            return nil;
    }
    // a kept pass's depth/stencil source header (RMPass::Zeta::sourceTic) is over the memory this moved from
    rmPassTrim();
    // published with submissions excluded, so a caller that waited on them finds this twin rather than moving again
    return twinPublish(t, t->_twin, twin);
}

id storedZetaTwin(RMTexture *t, id<MTLDevice> m2v, const char **why)
{
    return rmExcludingSubmissions([&]() -> id {
        return storedZetaTwinExcluded(t, m2v, why);
    });
}

bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, raw::SamplePositions &out)
{
    const auto read = [pass](float *xy, uint32_t n) {
        MTLSamplePosition at[8];
        [pass getSamplePositions:at count:n];
        for (uint32_t i = 0; i < n; i++) {
            xy[2 * i] = at[i].x;
            xy[2 * i + 1] = at[i].y;
        }
    };
    if (!raw::readSamplePositions(uint32_t(std::min<NSUInteger>(count, 9)), samples, read, out))
        return false;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log(rawLog(),
               "raw: a pass's own sample positions on (first: %u of %u samples, x | y << 4 %02x %02x %02x %02x)",
               unsigned(out.count), samples, out.xy[0], out.xy[1], out.xy[2], out.xy[3]);
    return true;
}
