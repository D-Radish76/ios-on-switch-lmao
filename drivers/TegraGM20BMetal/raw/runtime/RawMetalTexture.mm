// SPDX-License-Identifier: MIT
// A raw texture: its memory's lifetime, its m2v twin, and what Metal answers of it
#import "RawMetalInternal.h"
#import "RawMetalFresh.h"
#import "RawMetalMultisample.h"

// Before the raw path first renders into a surface texture: the kernel takes the surface's imports as only sampled until
// then, so a CPU lock of a surface the GPU never writes invalidates nothing.
void declareGpuWrites(RMTexture *t)
{
    // every pass over a surface target asks: only the first may exchange
    if (!t->_surface || t->_gpuWrites.load(std::memory_order_acquire) || t->_gpuWrites.exchange(true))
        return;
    if (gRM && gRM->raw && gRM->raw->surfaceGpuWrites(IOSurfaceGetID(t->_surface)))
        return;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log_error(rawLog(),
                     "raw: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
                     IOSurfaceGetID(t->_surface));
}

#if SWITCH_FRAME_TIMELINE
// A YCbCr or planar surface's image, made from the surface's planes by GPU work at the seed it had then: when its texture
// is made and at each conversion, with the seed before (none: never converted). The kernel's IOSurface lines name the
// CPU locks and writing unlocks of the same surface ID around it; a seed that moves with no unlock there is a writer the
// locks never see (a hardware engine, or a CPU write without a lock).
void traceSurfaceImage(RMTexture *t, const char *what, uint32_t seed)
{
    if (!t->_surface)
        return;
    const OSType f = IOSurfaceGetPixelFormat(t->_surface);
    os_log(rawLog(), "raw: IOSurface %u %ux%u '%c%c%c%c' image %s at seed %u (before: %s%u)",
           IOSurfaceGetID(t->_surface), unsigned(IOSurfaceGetWidth(t->_surface)),
           unsigned(IOSurfaceGetHeight(t->_surface)), char(f >> 24), char(f >> 16), char(f >> 8), char(f), what, seed,
           t->_imageCurrent ? "" : "none ", t->_imageCurrent ? t->_imageSeed : 0u);
}
#endif

@implementation RMTexture

- (instancetype)init
{
    if ((self = [super init]))
        _identity = rmNewIdentity();
    return self;
}

+ (Class)twinClass
{
    return NSClassFromString(@"NBTexture");
}

// SPI: the descriptor's (0 for every texture the raw path keeps, rawShape)
- (NSUInteger)rotation
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [_descriptor respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(_descriptor, rotation)
                                                     : 0;
}

- (void)dealloc
{
    _twin = nil;
    if (_heap && !_raw.arena) {
        _heap->_used -= _heapBytes;
        const raw::Texture::StorageRange storage = _raw.storageRange();
        rmHeapForget(_heap, storage.bytes ? storage.va : _raw.va);
    }
    if (_rawOk && _surface && _raw.converted())
        surfaceConverted(_surfaceWrites, false);
    rmHeapForgetTexture(self);
    if (_rawOk)
        if (gRM)
            gRM->raw->destroyTexture(_raw);
    if (_surface) {
        releaseSurfaceWrites(_surface, _surfaceWrites);
        CFRelease(_surface);
    }
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    id<MTLDevice> m2v = RM_M2V(NeedTextureTwin);
    id made = nil;  // outside this object's lock (twinPublish)
    const char *why = "m2v made none";
    {
        switch (_kind) {
        case RMTextureSurface:
            made = [m2v newTextureWithDescriptor:_descriptor iosurface:_surface plane:_plane];
            break;
        case RMTexturePool: {  // m2v's in-place texture over the same pool memory (as an IOSurface's)
            rmTexturePlain(self);
            // zeta memory of its own (raw::Device::createOwnedZeta): only a range that keeps none of it replays (ownedZetaIn)
            if (_raw.zeta && (_raw.samples > 1 || _raw.arena)) {
                made = [m2v newTextureWithDescriptor:_descriptor];
                break;
            }
            if (zetaDescriptor(
                    _descriptor)) {  // a stored one-sample zeta, moved into the twin's image (storedZetaTwin)
                made = _rawOk ? storedZetaTwin(self, m2v, &why) : [m2v newTextureWithDescriptor:_descriptor];
                break;
            }
            const size_t page = size_t(getpagesize());
            raw::Alias a;
            // Made aliasable before its first replay: its placement is given back, its address and its heap's arena kept
            const bool released = _raw.arena && !_raw.span.valid() && _heap && _heap->_arena;
            uint8_t *cpu = released ? _raw.cpu : _raw.span.cpu;
            uint64_t bytes = _raw.span.bytes, align;
            const auto releasedBytes = [&] {
                raw::TextureAsk ask;
                if (!heapTextureAsk(_descriptor, _heap->_descriptor.storageMode == MTLStorageModePrivate, ask))
                    return false;
                ask.standIn = false;
                return raw::Device::footprint(ask, bytes, align);
            };
            const bool shared =
                heapResident(_heap) &&
                (released ? releasedBytes() && gRM->raw->aliasArenaRange(_heap->_arena - 1, _raw.va, bytes, a)
                          : gRM->raw->alias(_raw, a));
            id<MTLBuffer> pages = shared ? rawAliasBuffer(a, cpu, (NSUInteger(bytes) + page - 1) & ~(page - 1),
                                                          MTLResourceStorageModeShared)
                                         : nil;
            why = _raw.zeta && _raw.samples > 1     ? "NVK views no multisampled zeta memory the raw path owns"
                  : !_raw.span.valid() && !released ? "it has no memory of its own"
                  : !shared ? "its memory could not be aliased (a purged heap's that did not come back)"
                  : !pages  ? "m2v did not take its memory"
                            : "m2v made no texture over its memory";
            MTLTextureDescriptor *d = _descriptor;
            if (_raw.chain && d.storageMode == MTLStorageModeMemoryless) {  // every level, in place (memory here)
                d = [_descriptor copy];
                d.storageMode = MTLStorageModePrivate;
            }
            if (pages && _raw.blockLinear && native_bridge_metal_texture_tiled_in_place)
                made = native_bridge_metal_texture_tiled_in_place(pages, d, 0);
            else if (pages && !_raw.blockLinear && native_bridge_metal_texture_in_place)
                made = native_bridge_metal_texture_in_place(pages, d, 0, _raw.pitch);
            break;
        }
        case RMTextureBuffer:
            if (id<MTLBuffer> b = [_buffer rmTwin]) {
                made = native_bridge_metal_texture_in_place
                           ? native_bridge_metal_texture_in_place(b, _descriptor, _offset, _rowBytes)
                           : [b newTextureWithDescriptor:_descriptor offset:_offset bytesPerRow:_rowBytes];
            }
            break;
        case RMTextureView:
            made = _viewPart ? [(id<MTLTexture>)[_parent rmTwin] newTextureViewWithPixelFormat:_descriptor.pixelFormat
                                                                                   textureType:_descriptor.textureType
                                                                                        levels:_viewLevels
                                                                                        slices:_viewSlices]
                             : [(id<MTLTexture>)[_parent rmTwin] newTextureViewWithPixelFormat:_descriptor.pixelFormat];
            if (made && !identitySwizzle(_descriptor.swizzle))  // m2v's texture makes no swizzled view
                SWITCH_REFUSE_DETAIL("raw", "tex.swizzle", _descriptor.pixelFormat,
                                     "a swizzled view of format %lu: m2v's work samples it unswizzled",
                                     (unsigned long)_descriptor.pixelFormat);
            break;
        case RMTextureMemoryless:
            made = [m2v newTextureWithDescriptor:_descriptor];
            break;
        case RMTextureTwinOnly:
            break;
        }
    }
    const char *error = !made && nmb_last_error ? nmb_last_error() : nullptr;
    id twin = twinPublish(self, _twin, made);
    if (!twin)
        SWITCH_REFUSE_DETAIL("raw", "twin.texture", _descriptor.pixelFormat,
                             "no m2v twin for a %ux%u texture of format %lu (kind %u%s): %s (%s)", _raw.width,
                             _raw.height, (unsigned long)_descriptor.pixelFormat, unsigned(_kind),
                             _raw.arena ? ", in a heap" : "", why, error && *error ? error : "-");
    return twin;
}

// Answered here, not by the twin (forwarded, each would make one): as m2v answers them
- (MTLTextureCompressionType)compressionType
{
    return _descriptor.compressionType;
}

- (BOOL)isCompressed
{
    return NO;
}

- (BOOL)isDrawable
{
    return NO;
}

- (MTLTextureSwizzleChannels)swizzle
{
    return _descriptor.swizzle;
}

- (NSUInteger)heapOffset
{
    return _raw.arena && _raw.span.valid() ? NSUInteger(_raw.span.offset) : 0;
}

// Answered here, not by the twin (a residency set's allocatedSize sums its allocations'): the memory this texture owns. A view and
// a texture over a buffer own none; a twin-only texture's is its twin's.
- (NSUInteger)allocatedSize
{
    if (_kind == RMTextureView || _kind == RMTextureBuffer || _raw.memoryless)
        return 0;
    if (_surface)
        return IOSurfaceGetAllocSize(_surface);
    if (_raw.span.valid())
        return NSUInteger(_raw.span.bytes);
    id twin = _twin;
    return twin ? [(id<MTLTexture>)twin allocatedSize] : NSUInteger(_raw.storage.bytes);
}

- (MTLTextureType)textureType
{
    return _descriptor.textureType;
}

- (MTLPixelFormat)pixelFormat
{
    return _descriptor.pixelFormat;
}

- (NSUInteger)width
{
    return _descriptor.width;
}

- (NSUInteger)height
{
    return _descriptor.height;
}

- (NSUInteger)depth
{
    return _descriptor.depth;
}

- (NSUInteger)mipmapLevelCount
{
    return _descriptor.mipmapLevelCount;
}

- (NSUInteger)arrayLength
{
    return _descriptor.arrayLength;
}

- (NSUInteger)sampleCount
{
    return _descriptor.sampleCount;
}

- (MTLTextureUsage)usage
{
    return _descriptor.usage;
}

- (MTLStorageMode)storageMode
{
    return _descriptor.storageMode;
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return _descriptor.cpuCacheMode;
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return _descriptor.hazardTrackingMode;
}

- (MTLResourceOptions)resourceOptions
{
    return _descriptor.resourceOptions;
}

- (BOOL)isFramebufferOnly
{
    return NO;
}

- (BOOL)allowGPUOptimizedContents
{
    return _descriptor.allowGPUOptimizedContents;
}

- (IOSurfaceRef)iosurface
{
    return _surface;
}

- (NSUInteger)iosurfacePlane
{
    return _plane;
}

- (id<MTLTexture>)parentTexture
{
    return (id<MTLTexture>)_parent;
}

- (id<MTLBuffer>)buffer
{
    return (id<MTLBuffer>)_buffer;
}

- (NSUInteger)bufferOffset
{
    return _offset;
}

- (NSUInteger)bufferBytesPerRow
{
    return _rowBytes;
}

- (NSUInteger)parentRelativeLevel
{
    return 0;
}

- (NSUInteger)parentRelativeSlice
{
    return 0;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    if (_heap)
        return [(id<MTLHeap>)_heap setPurgeableState:MTLPurgeableStateKeepCurrent];
    const MTLPurgeableState old = _purgeable ? _purgeable : MTLPurgeableStateNonVolatile;
    if (state != MTLPurgeableStateKeepCurrent)
        _purgeable = state == MTLPurgeableStateEmpty ? MTLPurgeableStateVolatile : state;
    return old;
}

- (void)makeAliasable
{
    if (!_heap || _aliasable)
        return;
    _aliasable = true;
    if (_raw.arena)
        gRM->raw->makeArenaAliasable(_raw);
    _heap->_used -= _heapBytes;
    _heapBytes = 0;
}

- (BOOL)isAliasable
{
    return _aliasable;
}

- (id<MTLHeap>)heap
{
    return (id<MTLHeap>)_heap;
}

bool identitySwizzle(MTLTextureSwizzleChannels s)
{
    return s.red == MTLTextureSwizzleRed && s.green == MTLTextureSwizzleGreen && s.blue == MTLTextureSwizzleBlue &&
           s.alpha == MTLTextureSwizzleAlpha;
}

id<MTLTexture> swizzled(id<MTLTexture> t, MTLTextureSwizzleChannels s)
{
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    if (!x || identitySwizzle(s))
        return t;
    x->_descriptor.swizzle = s;
    const uint8_t channels[4] = {uint8_t(s.red), uint8_t(s.green), uint8_t(s.blue), uint8_t(s.alpha)};
    if (x->_rawOk)
        gRM->raw->swizzle(x->_raw, channels);
    return t;
}

@end
