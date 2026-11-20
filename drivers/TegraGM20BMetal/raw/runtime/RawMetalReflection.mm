// SPDX-License-Identifier: MIT
// See RawMetalReflection.h. The objects a blob is read into are RawMetalReflectionObjects.mm's.
#import "RawMetalReflection.h"
#import "RawMetalComputeState.h"
#import "RawMetalInternal.h"

#include "raw_compute_key.h"
#include "raw_kernel_texture.h"
#include "raw_reflection_blob.h"

static bool reflectionAsked(MTLPipelineOption o)
{
    return o & (MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo);
}

static id fromBlob(const std::string &blob, bool compute)
{
    return rmReflectionOf(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), compute);
}

MTLAutoreleasedRenderPipelineReflection rmRenderReflection(RMPipeline *p, MTLRenderPipelineDescriptor *d,
                                                           MTLPipelineOption o)
{
    if (!reflectionAsked(o))
        return nil;
    std::string blob, why = "a function of another device, which no request can name";
    if (p->_completable) {
        uint32_t length = 0;
        if (const uint8_t *packed = gRM->raw->pack().reflection(p->_packKey, false, length))
            return rmReflectionOf(packed, length, false);
        if (gRM->completion && gRM->completion->reflection(rmRecordAsk(p, d), blob, why))
            if (id made = fromBlob(blob, false))
                return made;
    }
    os_log_error(rawLog(), "raw: %{public}s: no reflection of a render state, none given: %{public}s (%{public}s)",
                 getprogname(), why.c_str(), d.label.UTF8String ?: "-");
    return nil;
}

uint32_t rmComputeTexturesWritten(RMComputeState *s)
{
    std::call_once(s->_writtenOnce, [s] {
        s->_texturesWritten = ~0u;
        const std::string key = raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth);
        uint32_t length = 0;
        std::string blob, why;
        const void *bytes = gRM->raw->pack().reflection(raw::Pack::fnv(key.data(), key.size()), true, length);
        if (!bytes && gRM->completion && gRM->completion->reflection(s->_function, s->_direct, key, blob, why)) {
            bytes = blob.data();
            length = uint32_t(blob.size());
        }
        raw::ReflectionView v;
        if (bytes && raw::readReflection(bytes, length, raw::kReflectionComputeStages, v))
            s->_texturesWritten = raw::kernelTexturesWritten(v);
    });
    return s->_texturesWritten;
}

bool rmComputeReflection(RMComputeState *s, MTLPipelineOption o, MTLAutoreleasedComputePipelineReflection *reflection,
                         NSError **error)
{
    *reflection = nil;
    if (!reflectionAsked(o))
        return true;
    const std::string key = raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth);
    uint32_t length = 0;
    std::string blob, why = "no completion";
    if (const uint8_t *packed = gRM->raw->pack().reflection(raw::Pack::fnv(key.data(), key.size()), true, length))
        *reflection = rmReflectionOf(packed, length, true);
    else if (gRM->completion && gRM->completion->reflection(s->_function, s->_direct, key, blob, why))
        *reflection = fromBlob(blob, true);
    if (*reflection)
        return true;
    os_log_error(rawLog(), "raw: %{public}s: no reflection of a compute state: %{public}s", getprogname(), why.c_str());
    if (error) {
        NSString *text = [NSString stringWithFormat:@"no reflection: %s", why.c_str()];
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorInternal
                                 userInfo:@{NSLocalizedDescriptionKey : text}];
    }
    return false;
}
