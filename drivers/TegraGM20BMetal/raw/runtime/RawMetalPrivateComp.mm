// SPDX-License-Identifier: MIT
// The facade's half of private textures on compressed memory (raw_private_comp.h): which descriptors qualify, the move to
// plain memory before anything names a texture's pages another way, and the submit worker's moves onto compressed memory.
#import "RawMetalInternal.h"
#import "RawMetalPipeline.h"
#include <algorithm>
#include <vector>
#include "raw_submit_gate.h"
#include "raw_view.h"
#include "raw_xr.h"

thread_local bool tRawEncoding = false;

static raw::SubmitGate gSubmitGate;
static thread_local bool tSubmitting = false;

bool submitRangeExclusive(RMCommandBuffer *cb, bool followed)
{
    tSubmitting = true;
    const bool submitted = gSubmitGate.submit([&] {
        return submitRange(cb, followed);
    });
    tSubmitting = false;
    return submitted;
}

void rmMarkSubmission(bool submitting)
{
    tSubmitting = submitting;
}

raw::SubmitGate &rmSubmitGate()
{
    return gSubmitGate;
}

bool rmInSubmission()
{
    return tSubmitting;
}

uint32_t privateCompressedKind(MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    raw::PrivateTextureFacts facts;
    facts.privateStorage = d.storageMode == MTLStorageModePrivate;
    facts.levels = uint32_t(d.mipmapLevelCount);
    facts.samples = uint32_t(std::max<NSUInteger>(d.sampleCount, 1));
    facts.shape = d.textureType == MTLTextureType2D              ? raw::PrivateShape2D::Plain
                  : d.textureType == MTLTextureType2DArray       ? raw::PrivateShape2D::Array
                  : d.textureType == MTLTextureType2DMultisample ? raw::PrivateShape2D::Multisample
                                                                 : raw::PrivateShape2D::Other;
    facts.compressibleUsage =
        d.usage != MTLTextureUsageUnknown &&
        !(d.usage & ~NSUInteger(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite));
    facts.plainColour =
        f && f->rt && !raw::blockCompressed(f) && raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None;
    facts.bpp = f ? f->bpp : 0;
    facts.compression = (gRM->raw->kernelInfo().flags & TegraGM20BInfoCompression) != 0;
    return raw::privateCompressedKind(facts);
}

bool rmTexturePlain(RMTexture *t)
{
    if (!t || !t->_rawOk || !gRM->raw->privateCompressed(t->_raw))
        return true;
    return rmExcludingSubmissions([t]() -> bool {
        if (tRawEncoding) {
            SWITCH_REFUSE_DETAIL("raw", "comp.private", t->_descriptor.pixelFormat,
                                 "a compressed %ux%u texture of format %lu was asked for its pages inside a raw encode",
                                 t->_raw.width, t->_raw.height, (unsigned long)t->_descriptor.pixelFormat);
            return false;
        }
        @synchronized(t) {
            if (!gRM->raw->privateCompressed(t->_raw))
                return true;
            if (!gRM->raw->plainFromCompressed(t->_raw))
                return false;
        }
        return true;
    });
}

void rmWantCompressed(id<MTLTexture> texture)
{
    RMTexture *t = [(id)texture isKindOfClass:[RMTexture class]] ? (RMTexture *)texture : nil;
    if (!t || !t->_movableBytes)
        return;
    t->_wanting = t;
    if (!gRM->raw->wantCompressed(t->_raw, privateCompressedKind(t->_descriptor), (__bridge void *)t))
        t->_movableBytes = 0;
}

static std::vector<RMTexture *> &gMoving = *new std::vector<RMTexture *>;  // the submit worker's alone

// Called under the device lock (Device::privateMoves): a texture whose want is still there has not reached the
// destroyTexture of its dealloc, which erases it under that lock, so its memory holds; one being deallocated loads nil
static void takeWanting(void *owner)
{
    __unsafe_unretained RMTexture *t = (__bridge RMTexture *)owner;
    if (RMTexture *alive = t->_wanting)
        gMoving.push_back(alive);
}

void rmMoveWanting()
{
    raw::Device &d = *gRM->raw;
    if (!d.privateMoveDue())
        return;
    d.privateMoves(takeWanting);
    if (gMoving.empty())
        return;
    // a move's job is a command buffer of its own: an encode ahead holding the device's goes back first
    rmPipelineYield(nil, SIZE_MAX, false);
    for (RMTexture *t : gMoving)
        d.compressedFromPlain(t->_raw);
    gMoving.clear();
}
