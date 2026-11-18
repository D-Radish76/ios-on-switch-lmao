// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_srgb_pair.h"

// Each predicate's refusal key: domain "raw", the predicate's name as the reason (kits/SwitchRefusal)
static constexpr std::array<uint64_t, PredCount> predKeys()
{
    std::array<uint64_t, PredCount> keys{};
    for (int p = 0; p < PredCount; p++)
        keys[p] = switch_refusal_key("raw", kPredNames[p]);
    return keys;
}

static constexpr std::array<uint64_t, PredCount> kPredKeys = predKeys();
#if SWITCH_RAW_DIAGNOSTICS
std::atomic<uint64_t> gPredFailed[PredCount];
#endif
std::atomic<uint64_t> gPredLogged{0};

// Each predicate's first hit in this process, through kits/SwitchRefusal (domain raw-metal, the predicate's name as the
// reason)
void logPred(RMPred p, uint64_t a, uint64_t b, NSString *detail)
{
    const uint64_t bit = 1ull << p;
    if (gPredLogged.fetch_or(bit, std::memory_order_relaxed) & bit)
        return;
    switch_refusal_k(kPredKeys[p], "raw", kPredNames[p], "a=%llu (0x%llx) b=%llu (0x%llx)%s%s", (unsigned long long)a,
                     (unsigned long long)a, (unsigned long long)b, (unsigned long long)b, detail ? " " : "",
                     detail ? detail.UTF8String : "");
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: first %s: a=%llu b=%llu %s\n", kPredNames[p], (unsigned long long)a, (unsigned long long)b,
            detail ? detail.UTF8String : "");
#endif
}

void predFails(RMCommandBuffer *cb, RMPred p, uint64_t a, uint64_t b, const char *detail)
{
    const NSInteger code = predFailure(p);
    gPredLogged.fetch_or(1ull << p, std::memory_order_relaxed);
    switch_refusal_k(kPredKeys[p], "raw", kPredNames[p],
                     "the command buffer fails (MTLCommandBufferErrorDomain %ld) a=%llu (0x%llx) b=%llu (0x%llx)%s%s",
                     long(code), (unsigned long long)a, (unsigned long long)a, (unsigned long long)b,
                     (unsigned long long)b, detail ? " " : "", detail ?: "");
    RAW_DIAGNOSTIC(gPredFailed[p].fetch_add(1, std::memory_order_relaxed));
    if (!cb->_failedBy)
        cb->_failedBy = p;
    failCommandBuffer(
        cb, [NSString stringWithFormat:@"The raw path refused %s%s%s", kPredNames[p], detail ? ": " : "", detail ?: ""],
        MTLCommandBufferError(code));
}

// The type encoding of a selector some MTL* protocol declares (the plugin's answerUnimplemented uses the same lookup), walked
// once per selector: objc_copyProtocolList copies the shared cache's protocols (the frontend's forwardSignature, r14-maps)
const char *metalTypes(SEL selector)
{
    static raw::Mutex lock;
    static auto *known = new std::unordered_map<SEL, const char *>;
    {
        std::lock_guard<raw::Mutex> l(lock);
        if (const auto it = known->find(selector); it != known->end())
            return it->second;
    }
    unsigned count = 0;
    Protocol *__unsafe_unretained *protocols = objc_copyProtocolList(&count);
    const char *types = nullptr;
    for (unsigned i = 0; i < count && !types; ++i) {
        if (strncmp(protocol_getName(protocols[i]), "MTL", 3) != 0)
            continue;
        for (int required = 1; required >= 0 && !types; --required) {
            const objc_method_description d = protocol_getMethodDescription(protocols[i], selector, required, YES);
            if (d.name)
                types = d.types;
        }
    }
    free(protocols);
    std::lock_guard<raw::Mutex> l(lock);
    known->emplace(selector, types);
    return types;
}

bool twinAnswers(Class twinClass, SEL selector)
{
    return twinClass && [twinClass instancesRespondToSelector:selector];
}

static void returnZero(NSInvocation *invocation)
{
    if (const NSUInteger length = invocation.methodSignature.methodReturnLength) {
        uint64_t small[8] = {};
        void *zero = length <= sizeof small ? small : calloc(1, length);
        [invocation setReturnValue:zero];
        if (zero != small)
            free(zero);
    }
}

void answerZero(id object, NSInvocation *invocation)
{
    Class cls = object_getClass(object);
    SWITCH_REFUSE_DETAIL("raw", "selector.zero", uintptr_t(invocation.selector) * 31 + uintptr_t(cls),
                         "-[%s %s] answered zero, as m2v's object answers it", class_getName(cls),
                         sel_getName(invocation.selector));
#if SWITCH_FRAME_TIMELINE
    if (gRM)
        RAW_DIAGNOSTIC(gRM->stats.zeroAnswers.fetch_add(1, std::memory_order_relaxed));
#endif
    returnZero(invocation);
}

void answerMissing(id object, NSInvocation *invocation)
{
    Class cls = object_getClass(object);
    const uint64_t key = switch_refusal_key_with(SWITCH_REFUSAL_KEY("raw", "selector.missing"),
                                                 uintptr_t(invocation.selector) * 31 + uintptr_t(cls));
    if (switch_refusal_first(key)) {
        char text[256];
        snprintf(text, sizeof text, "-[%s %s] has no raw method (m2v's object has one): answered zero",
                 class_getName(cls), sel_getName(invocation.selector));
        switch_refusal_log("raw", "selector.missing", text);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        os_log_fault(rawLog(), "raw: %{public}s", text);
#endif
    }
#if SWITCH_FRAME_TIMELINE
    if (gRM)
        RAW_DIAGNOSTIC(gRM->stats.twinForwards.fetch_add(1, std::memory_order_relaxed));
#endif
    returnZero(invocation);
}

raw::shapes::TextureShape textureShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    raw::shapes::TextureShape s;
    s.type = uint32_t(d.textureType);
    s.format = uint32_t(d.pixelFormat);
    s.storage = uint32_t(d.storageMode);
    s.usage = d.usage;
    s.width = d.width;
    s.height = d.height;
    s.depth = d.depth;
    s.levels = d.mipmapLevelCount;
    s.samples = d.sampleCount;
    s.array = d.arrayLength;
    s.rotation = [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return s;
}

bool rawShape(MTLTextureDescriptor *d)
{
    return raw::shapes::rawShape(textureShape(d));
}

bool chainShape(MTLTextureDescriptor *d)
{
    return raw::shapes::chainShape(textureShape(d));
}

bool lineShape(MTLTextureDescriptor *d)
{
    return raw::shapes::lineShape(textureShape(d));
}

bool volumeRendered(MTLTextureDescriptor *d)
{
    return raw::shapes::volumeRendered(textureShape(d));
}

bool volumeShape(MTLTextureDescriptor *d)
{
    return raw::shapes::volumeShape(textureShape(d));
}

uint32_t arrayType(MTLTextureDescriptor *d)
{
    return raw::shapes::arrayType(textureShape(d));
}

bool zetaDescriptor(MTLTextureDescriptor *d)
{
    return raw::shapes::zetaShape(textureShape(d));
}

bool multisampleShape(MTLTextureDescriptor *d)
{
    return raw::shapes::multisampleShape(textureShape(d));
}

uint32_t memorylessLayers(MTLTextureDescriptor *d)
{
    return raw::shapes::memorylessLayers(textureShape(d));
}

bool blockShape(MTLTextureDescriptor *d)
{
    return raw::shapes::blockShape(textureShape(d));
}

uint32_t rawShapeTerm(MTLTextureDescriptor *d)
{
    return raw::shapes::rawShapeTerm(textureShape(d));
}

// A kept extended-range texture's facts (raw_xr.h), or an RG8Unorm_sRGB one's stand-in (raw_srgb_pair.h). Metal keeps
// it compressed with allowGPUOptimizedContents, 16 texels or more a side and no ShaderWrite, PixelFormatView or
// ShaderAtomic usage, never over a buffer (m2v heap_tiled.rs metal_compresses); one draws may sample gets its stand-in.
// False: the memory or header of a stand-in it needs was refused, so no draw could sample it. A pack with no conversion
// state for the form (a host test's) makes it without one, which a draw sampling it refuses at its bind.
bool keepExtendedRange(MTLTextureDescriptor *d, raw::Texture &t, bool overBuffer)
{
    const bool sampled = d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageShaderRead);
    if (raw::srgbPair(t.format))
        return !sampled || gRM->raw->createStandIn(t);
    if (!raw::xrSampled(t.format))
        return true;
    const MTLTextureUsage plain =
        MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView | MTLTextureUsageShaderAtomic;
    const bool every = d.usage == MTLTextureUsageUnknown;
    t.compressed =
        !overBuffer && d.allowGPUOptimizedContents && std::min(d.width, d.height) >= 16 && !every && !(d.usage & plain);
    return !sampled || gRM->raw->createStandIn(t) || !gRM->raw->ownPipeline(raw::xrConversionOf(t.format, t.xrPlanes));
}

