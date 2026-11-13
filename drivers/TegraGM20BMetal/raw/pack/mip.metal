// SPDX-License-Identifier: MIT
// raw GM20B path: generateMipmapsForTexture: steps the 2D engine is not proven within one code of Metal's at
// (runtime/raw_mip.h, raw/fx/fx_mip_route.h), one pass a level drawn into that level from the one above, one pixel a
// fragment. Each texel is Metal's step: one bilinear tap at its centre mapped onto the level above, ((2x + 1) s - d) / 2d
// source texels for an axis of s texels halved to d, the weight rounded to the nearest 1/256, blended from the four texels
// read whole: the texture unit's filter takes fractions off the quarters at weights of its own (the 2D engine's put odd
// steps up to 284 half ulps off Metal's on the chip, boot 21 gpu.raw.mip_twod rgba16f_odd_max_diff). An sRGB level is read
// as its codes (a header without the sRGB conversion, Device::levelTic) and decoded here, then blended in linear light: the
// texture unit's decode put proven steps two codes off Metal's (boot 21 srgb=bgra8srgb:3d=57/2). The target encodes what
// the fragment writes.
// Buffer 0 holds the step: the size of the level above, then of this level.
// One function a target format: the pack keys an own state by its function's name.
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out mip_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

struct Step {
    uint2 above, size;
};

struct Taps {
    uint2 first, second;
    float2 weight;
};

static Taps taps(float2 position, constant Step &step)
{
    const uint2 x = uint2(position), s = step.above, d = step.size;
    const uint2 at = (2 * x + 1) * s - d;
    const uint2 whole = at / (2 * d), part = at % (2 * d);
    Taps t;
    t.first = min(whole, s - 1);
    t.second = min(whole + 1, s - 1);
    t.weight = float2((part * 256 + d) / (2 * d)) / 256.0;
    return t;
}

static float4 blend(float4 a, float4 b, float4 c, float4 e, float2 w)
{
    const float4 top = fma(b, float4(w.x), a * (1.0 - w.x)), bottom = fma(e, float4(w.x), c * (1.0 - w.x));
    return fma(bottom, float4(w.y), top * (1.0 - w.y));
}

static float4 linearLight(float4 code)
{
    const float3 c = code.rgb;
    return float4(select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045), code.a);
}

static float4 levelStep(Out in, texture2d<float> image, constant Step &step)
{
    const Taps t = taps(in.position.xy, step);
    return blend(image.read(t.first), image.read(uint2(t.second.x, t.first.y)), image.read(uint2(t.first.x, t.second.y)),
                 image.read(t.second), t.weight);
}

static float4 srgbStep(Out in, texture2d<float> codes, constant Step &step)
{
    const Taps t = taps(in.position.xy, step);
    return blend(linearLight(codes.read(t.first)), linearLight(codes.read(uint2(t.second.x, t.first.y))),
                 linearLight(codes.read(uint2(t.first.x, t.second.y))), linearLight(codes.read(t.second)), t.weight);
}

#define MIP(name, form)                                                                                                       \
    fragment float4 mip_##name##_frag(Out in [[stage_in]], texture2d<float> image [[texture(0)]],                         \
                                      constant Step &step [[buffer(0)]])                                                     \
    {                                                                                                                         \
        return form(in, image, step);                                                                                         \
    }

MIP(a8, levelStep)
MIP(r8, levelStep)
MIP(r16, levelStep)
MIP(r16f, levelStep)
MIP(rg8, levelStep)
MIP(b5g6r5, levelStep)
MIP(bgr5a1, levelStep)
MIP(r32f, levelStep)
MIP(rg16, levelStep)
MIP(rg16f, levelStep)
MIP(rgba8, levelStep)
MIP(bgra8, levelStep)
MIP(rgba8_srgb, srgbStep)
MIP(bgra8_srgb, srgbStep)
MIP(rgb10a2, levelStep)
MIP(rg11b10f, levelStep)
MIP(bgr10a2, levelStep)
MIP(rg32f, levelStep)
MIP(rgba16, levelStep)
MIP(rgba16f, levelStep)
MIP(rgba32f, levelStep)
