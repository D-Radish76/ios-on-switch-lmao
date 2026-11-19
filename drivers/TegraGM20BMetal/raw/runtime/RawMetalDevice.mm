// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalRotated.h"
#import "RawMetalXrPlanes.h"
#import "RawMetalFresh.h"
#include "raw_surface_note.h"
#include "raw_surface_refusal.h"
#include <mach/vm_page_size.h>
#import "RawMetalMultisample.h"

@implementation RMDevice

- (void)rmCommit:(RMCommandBuffer *)cb
{
    commitBuffer(cb);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's attribution: this process's command buffers since the device opened, never reset (the rates line's
// windows stay its own). Keys: raw, reopens (channels the kernel killed and this process opened again), reopen_us (the
// last one's time), set_refusals and set_rebuilds (resource set registrations the kernel refused, and those it took
// once made again without what it refused), pipelined_encoders and sealed_encoders (encoders the worker encoded ahead
// of their commit, of all it sealed), pipeline_handoffs and pipeline_handoff_wakes (hand-offs to the worker as encoders
// ended, and those that found it asleep: a wake each), pipeline_abandons.<reason>, failed (the command buffers that
// completed with an error) and failed.<predicate> (those by the predicate their failure named, failed.none where it
// named none; nonzero only, their sum `failed`), skipped (the calls Metal's validation stops that ran as Apple's driver
// runs them, not failures) and skipped.<predicate> (nonzero only, their sum `skipped`).
- (NSDictionary<NSString *, NSNumber *> *)switchRawCounters
{
    const Stats &s = gRM->stats;
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"raw"] = @(s.raw.load());
    d[@"reopens"] = @(gRM->raw->stats.reopens.load());
    const raw::ResourceSet::Counts sets = gRM->raw->setCounts();
    d[@"set_refusals"] = @(sets.refusals);
    d[@"set_rebuilds"] = @(sets.rebuilds);
    {
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        d[@"reopen_us"] = @(gRM->raw->stats.reopenTicks.load() * base.numer / base.denom / 1000);
    }
    d[@"pipelined_encoders"] = @(s.pipelinedEncoders.load());
    d[@"sealed_encoders"] = @(s.sealedEncoders.load());
    d[@"pipeline_handoffs"] = @(s.handoffs.load());
    d[@"pipeline_handoff_wakes"] = @(s.handoffWakes.load());
    static const char *const vetoes[VetoKinds] = {"-",        "counters", "shadows",    "variant", "probes",
                                                  "parallel", "ops",      "refused",    "planar",  "contents",
                                                  "native",   "failed",   "other_work", "device",  "zcull",
                                                  "channel"};
    for (uint32_t k = 0; k < VetoKinds; ++k)
        if (const uint64_t n = s.pipelineAbandons[k].load())
            d[[NSString stringWithFormat:@"pipeline_abandons.%s", vetoes[k]]] = @(n);
    d[@"split_prologues"] = @(gRM->raw->stats.splitLaunches.load());
    d[@"split_room_refusals"] = @(gRM->raw->stats.splitRoomRefusals.load());
    d[@"heap_full"] = @(gRM->raw->stats.heapFull.load());
    d[@"pack_slots_in_bound_grains"] = @(gRM->raw->stats.packSlotsInBoundGrains.load());
    d[@"store_slots_placed"] = @(gRM->raw->stats.storeSlotsPlaced.load());
    d[@"slot_cleans"] = @(gRM->raw->stats.slotCleans.load());
    d[@"clears_narrowed"] = @(gRM->raw->stats.clearsNarrowed.load());
    d[@"clear_pixels_left"] = @(gRM->raw->stats.clearPixelsLeft.load());
    d[@"interlock_draws"] = @(gRM->raw->stats.interlockDraws.load());
    d[@"raster_ordered_draws"] = @(gRM->raw->stats.rasterOrderedDraws.load());
    d[@"banded_draws"] = @(gRM->raw->stats.bandedDraws.load());
    d[@"bands"] = @(gRM->raw->stats.bands.load());
    d[@"learned_draws"] = @(gRM->raw->stats.learnedDraws.load());
    d[@"banded_dispatches"] = @(gRM->raw->stats.bandedDispatches.load());
    d[@"storage_barriers"] = @(gRM->raw->stats.storageBarriers.load());
    d[@"texture_uploads"] = @(gRM->raw->stats.textureUploads.load());
    d[@"fused_passes"] = @(gRM->raw->stats.fusedPasses.load());
    d[@"texture_readbacks"] = @(gRM->raw->stats.textureReadbacks.load());
    d[@"blend_kill_states"] = @(gRM->raw->stats.blendKillStates.load());
    d[@"blend_kill_binds"] = @(gRM->raw->stats.blendKillBinds.load());
    d[@"resolve_twod"] = @(gRM->raw->stats.resolveTwod.load());
    d[@"fill_twod"] = @(gRM->raw->stats.fillTwod.load());
    d[@"resolve_exact"] = @(gRM->raw->stats.resolveExact.load());
    d[@"resolve_twod_compressed"] = @(gRM->raw->stats.resolveTwodCompressed.load());
    d[@"zcull_passes"] = @(gRM->raw->stats.zcullPasses.load());
    d[@"zcull_invalidations"] = @(gRM->raw->stats.zcullInvalidations.load());
    d[@"zcull_loads"] = @(gRM->raw->stats.zcullLoads.load());
    d[@"state_segment_binds"] = @(gRM->raw->stats.segmentBinds.load());
    d[@"state_segment_words"] = @(gRM->raw->stats.segmentWords.load());
    d[@"state_segment_skips"] = @(gRM->raw->stats.segmentSkips.load());
    d[@"state_segment_extras_losses"] = @(gRM->raw->stats.segmentExtrasLosses.load());
    d[@"state_segment_copies"] = @(gRM->raw->stats.segmentCopies.load());
    {
        const raw::EncodeTotals &encode = gRM->raw->stats.encode;
        const double perUs = raw::encodeTicksPerUs();
        d[@"encodes"] = @(encode.encodes.load());
        d[@"encode_us"] = @(uint64_t(double(encode.wallTicks.load()) / perUs));
        d[@"exec_us"] = @(uint64_t(double(encode.ticks[size_t(raw::EncodePart::Exec)].load()) / perUs));
        d[@"exec_pushes"] = @(encode.execPushes.load());
    }
    d[@"replay_same"] = @(gRM->raw->stats.replay.same.load());
    d[@"replay_recent"] = @(gRM->raw->stats.replay.recent.load());
    d[@"replay_new"] = @(gRM->raw->stats.replay.fresh.load());
    d[@"stencil_bit_draws"] = @(gRM->raw->stats.stencilBitDraws.load());
    d[@"zeta_colour_draws"] = @(gRM->raw->stats.zetaColourDraws.load());
    d[@"mip_twod_levels"] = @(gRM->raw->stats.mipTwodLevels.load());
    d[@"mip_twod_unproven_levels"] = @(gRM->raw->stats.mipTwodUnproven.load());
    d[@"mip_3d_levels"] = @(gRM->raw->stats.mip3dLevels.load());
    d[@"private_compressed"] = @(gRM->raw->privateMade());
    d[@"private_moves"] = @(gRM->raw->privateTransitions());
#if SWITCH_RAW_DIAGNOSTICS
    d[@"loop_continuations"] = @(gRM->stats.loopContinuations.load());
#endif
    d[@"xr_plane_passes"] = @(gRM->raw->stats.xrPlanePasses.load());
    d[@"xr_plane_shadows"] = @(gRM->raw->stats.xrPlaneShadows.load());
    d[@"xr_pair_binds"] = @(gRM->raw->stats.xrPairBinds.load());
    const raw::FetchCounts &fetch = gRM->raw->stats.fetch;
    d[@"fetch_sequential"] = @(fetch.sequential.load());
    d[@"fetch_triangles"] = @(fetch.sequentialTriangles.load());
    d[@"fetch_interlocked"] = @(fetch.interlocked.load());
    d[@"fetch_rung2"] = @(fetch.fetchFree[0].load());
    d[@"fetch_rung3"] = @(fetch.fetchFree[1].load());
    d[@"fetch_blend_twin"] = @(fetch.blendTwin.load());
    d[@"fetch_twin_rule"] = @(fetch.twinRule.load());
    for (size_t r = 1; r < raw::kFetchRefusals; r++)
        if (const uint64_t n = fetch.refusedAtEncode[r].load())
            d[[NSString stringWithFormat:@"fetch_encode.%s", raw::kFetchRefusalNames[r]]] = @(n);
    for (size_t r = 1; r < raw::kStorageFallbacks; r++)
        if (const uint64_t n = fetch.storageFallback[r].load())
            d[[NSString stringWithFormat:@"fetch_fallback.%s", raw::kStorageFallbackNames[r]]] = @(n);
    uint64_t failed = 0;
    for (int p = 0; p < PredCount; p++)
        if (const uint64_t n = gPredFailed[p].load(std::memory_order_relaxed)) {
            failed += n;
            d[[NSString stringWithFormat:@"failed.%s", failedName(RMPred(p))]] = @(n);
        }
    d[@"failed"] = @(failed);
    uint64_t skippedCalls = 0;
    for (int p = 1; p < PredCount; p++)
        if (const uint64_t n = gPredSkipped[p].load(std::memory_order_relaxed)) {
            skippedCalls += n;
            d[[NSString stringWithFormat:@"skipped.%s", kPredNames[p]]] = @(n);
        }
    d[@"skipped"] = @(skippedCalls);
#if SWITCH_RAW_DIAGNOSTICS
    // states created that the pack lacked: found in this build's record cache or compiled (states_compiled)
    if (gRM->completion) {
        const raw::Compiler::Stats made = gRM->completion->stats();
        d[@"pack_misses"] = @(made.hits + made.misses);
        d[@"states_compiled"] = @(made.misses);
    }
#endif
    return d;
}

// hw_test_app's system.widgets_render and gpu.raw.memoryless_burst: the bytes the raw device holds now. pool: its pool,
// block-linear and depth chunks; wc: its write-combined chunks; heap: the shader heap's host memory (programs copied);
// heap_in_place: the pack's and the shader compiler's store's pages larger programs run from (file pages, outside the
// footprint); backings: memoryless attachments' uncompressed backings (trimBackings' last count); grows: allocations that
// wired a chunk themselves; headers_left: the texture headers not held (8191 at most, raw_gpu.h kTextureHeaders).
// hw_test_app's gpu.path.set_survives_trim: the latest resource set registration the kernel refused, its member and why
- (NSString *)switchRawSetRefusal
{
    return [NSString stringWithUTF8String:gRM->raw->setLastRefusal().c_str()];
}

- (NSDictionary<NSString *, NSNumber *> *)switchRawMemory
{
    raw::Device &d = *gRM->raw;
    return @{
        @"pool" : @(d.stats.poolBytes.load()),
        @"wc" : @(d.wcChunkBytesHeld()),
        @"heap" : @(d.heapBytes()),
        @"heap_in_place" : @(d.heapInPlaceBytes()),
        @"backings" : @(d.stats.backingBytes.load()),
        @"grows" : @(d.stats.poolStalls.load()),
        @"headers_left" : @(d.headersLeft())
    };
}

// hw_test_app's gpu.compression_churn and gpu.raw.compressed_targets: the raw device's compressed memory (raw_comp.h).
// held, held_bytes and views are now; made and refused count since the device opened, and so do the clears of compressed
// targets the ZBC table held or not and the entries this process added (raw_zbc.h); ce_copies: a shadow's fills and stores
// through compressed memory; storage_fallback_compressed: draws whose storage form a compressed attachment turned away.
- (NSDictionary<NSString *, NSNumber *> *)switchRawCompression
{
    const auto &s = gRM->raw->stats;
    return @{
        @"held" : @(s.compHeld.load()),
        @"held_bytes" : @(s.compHeldBytes.load()),
        @"views" : @(s.compViews.load()),
        @"made" : @(s.compMade.load()),
        @"refused" : @(s.compFailed.load()),
        @"zbc_hits" : @(s.zbcHits.load()),
        @"zbc_misses" : @(s.zbcMisses.load()),
        @"zbc_added" : @(s.zbcAdded.load()),
        @"zbc_full" : @(s.zbcFull.load()),
        @"zbc_inexact" : @(s.zbcInexact.load()),
        @"ce_copies" : @(s.compCopies.load()),
        @"zeta_ce_copies" : @(s.compZetaCopies.load()),
        @"storage_fallback_compressed" : @(s.fetch.storageFallback[size_t(raw::StorageFallback::Compressed)].load()),
        @"misses" : @(s.compMisses.load())
    };
}

// hw_test_app's gpu.raw.texel_buffer_reach: a texel buffer of `n` texels from `b`'s first byte past the device's
// maxTextureBufferWidth answer (NVK's 2^27; the header's width field holds 32 bits), nil where none is made
- (id<MTLTexture>)switchRawTexelBuffer:(id<MTLBuffer>)b format:(MTLPixelFormat)f width:(NSUInteger)n
{
    RMBuffer *buffer = [(id)b isKindOfClass:[RMBuffer class]] ? (RMBuffer *)b : nil;
    if (!buffer || n > UINT32_MAX)
        return nil;
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_kind = RMTextureBuffer;
    t->_buffer = buffer;
    t->_descriptor = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:f
                                                                           width:n
                                                                 resourceOptions:b.resourceOptions
                                                                           usage:MTLTextureUsageShaderRead];
    t->_rawOk = gRM->raw->createTexelBuffer(buffer->_raw, 0, uint32_t(n), uint32_t(f), t->_raw, UINT32_MAX);
    return t->_rawOk ? (id<MTLTexture>)t : nil;
}
#endif

// Alignments Core Animation lays its memory out by (QuartzCore 21H461), answered for what the raw path takes as it is:
//  - its buffer pool's suballocations (CA::OGL::MetalBufferPool::get_buffer: max(this, the argument's)): the A10X's 4, a
//    uniform buffer the raw path binds at the multiple of 256 below it with m2v's residual in its push word (raw::PackEntry);
//  (not iosurfaceReadOnlyTextureAlignmentBytes: QuartzCore makes it the render server's needed IOSurface row alignment,
//  CADeviceSetMinimumAlignment, and hands it to every client, CA::Context::connect_remote; MetalContext::update_surface then
//  refuses every surface whose rows miss it, "Row bytes (N) not 128 aligned", and the layer draws nothing (r7p5: 3347 refusals
//  in backboardd, Safari images and widgets blank). Its answer stays 16 (raw_device_answers.h), though the texture
//  header takes rows of 32 and a pitch render target rows of 128.)
//  - the image memory it samples in place (CA::OGL::MetalContext::copy_image_to_texture: offset and rows a multiple of
//    deviceLinearReadOnlyTextureAlignmentBytes, else it copies them into a texture of its own): the texture header's 32,
//    raw_device_answers.h's row;
//  - the rows of its CPU readbacks (CAMetalTextureGetBytesPerRow).
- (NSUInteger)minConstantBufferAlignmentBytes
{
    return 4;
}

- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format
{
    const NSUInteger a = NSUInteger(deviceAnswer(@selector(minimumLinearTextureAlignmentForPixelFormat:), format));
    return a ? std::max<NSUInteger>(a, 32) : a;
}

- (id<MTLCommandQueue>)newCommandQueue
{
    RMQueue *q = [RMQueue new];
    q->_device = self;
    return (id<MTLCommandQueue>)q;
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n
{
    (void)n;
    return [self newCommandQueue];
}

- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(id)d
{
    (void)d;
    return [self newCommandQueue];
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    // a shared buffer: write-combined (raw::Buffer::wc), as every one Core Animation makes is written by the CPU and read by the
    // GPU (its buffer pool, its gamma and weight tables: QuartzCore MetalBufferPool::allocate_buffer, options 0): nothing to clean
    // before a draw reads it, nothing to scan for the vertices an indexed draw reads (r16: 600-1300 KB cleaned a command buffer)
    const bool wc = (o & MTLResourceStorageModeMask) == MTLResourceStorageModeShared;
    if (!gRM->raw->createBuffer(n ? n : 16, b->_raw, wc)) {
        os_log_error(rawLog(), "raw: %{public}s: no memory for a %lu-byte buffer: none made", getprogname(),
                     (unsigned long)n);
        return nil;
    }
    b->_raw.hostVisible = (o & MTLResourceStorageModeMask) != MTLResourceStorageModePrivate;
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    id<MTLBuffer> b = [self newBufferWithLength:n options:o];
    if (!b || !p || !n)
        return b;
    RMBuffer *buffer = (RMBuffer *)b;
    raw::HostAccess access;
    if (!buffer->_raw.hostVisible && !gRM->raw->hostAccess(buffer->_raw, true, access))
        return nil;
    memcpy(buffer->_raw.cpu, p, n);
    if (!access.finish())
        return nil;
    return b;
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)p
                                   length:(NSUInteger)n
                                  options:(MTLResourceOptions)o
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
    SWITCH_API(ApiObject);
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    // The kernel wires pages the task maps read-only for GPU reads (TegraGM20BUserClientBo.cpp boImport): what it refuses
    // no device of this GPU imports
    if (!gRM->raw->createBufferNoCopy(p, n, b->_raw)) {
        os_log_error(rawLog(), "raw: %{public}s: the kernel imported no %lu bytes at %p: no no-copy buffer made",
                     getprogname(), (unsigned long)n, p);
        return nil;
    }
    b->_deallocator = [deallocator copy];
    return (id<MTLBuffer>)b;
}

// (SPI) A surface of one plane as a buffer over its pages, as m2v makes one: nil for a planar, unaligned or block-linear
// surface. The GPU's writes are declared at once, so a CPU lock waits for them.
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)s
{
    SWITCH_API(ApiObject);
    if (!s || IOSurfaceGetPlaneCount(s))
        return nil;
    void *base = IOSurfaceGetBaseAddress(s);
    const size_t page = vm_page_size, allocation = IOSurfaceGetAllocSize(s);
    if (!base || (uintptr_t(base) & (page - 1)) || !allocation || allocation > SIZE_MAX - (page - 1))
        return nil;
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = MTLResourceStorageModeShared;
    bool tiled = false;
    if (!gRM->raw->createSurfaceBuffer(IOSurfaceGetID(s), base, (allocation + page - 1) & ~(page - 1), s, b->_raw,
                                       tiled)) {
        if (!tiled)
            os_log_error(rawLog(), "raw: %{public}s: the kernel imported no IOSurface %u: no buffer over it made",
                         getprogname(), IOSurfaceGetID(s));
        return nil;
    }
    if (!gRM->raw->surfaceGpuWrites(IOSurfaceGetID(s)))
        os_log_error(rawLog(),
                     "raw: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
                     IOSurfaceGetID(s));
    return (id<MTLBuffer>)b;
}

#if SWITCH_FRAME_TIMELINE
static void noteCompositorTarget(MTLTextureDescriptor *d, const void *frame)
{
    constexpr NSUInteger kPanel = 1280 * 720;
    if (!(d.usage & MTLTextureUsageRenderTarget) || d.width * d.height <= kPanel || !gRM || !gRM->raw ||
        gRM->raw->role() != raw::Device::Role::RenderServer || !raw::noteAllowed())
        return;
    raw::postTargetNote({uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), uint32_t(d.usage),
                         uint32_t(d.storageMode), uint32_t(d.sampleCount), uint32_t(d.textureType),
                         switch_frame::callers(frame)});
}
#endif

// A private or shared texture of its own memory is block-linear (TegraGM20BLayout.h), as NVK lays out an optimal image and
// m2v's view of it is (texture_tiled_in_place). The CPU reaches a shared one only through replaceRegion and getBytes,
// which go through the copy engine (transferTexture) or swizzle (cpuTiled); textures over buffers, IOSurfaces and
// caller pages stay pitch, as another binding names those bytes.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
#if SWITCH_FRAME_TIMELINE
    noteCompositorTarget(d, __builtin_frame_address(0));
#endif
    return swizzled([self rmNewTexture:d
                                 tiled:d.storageMode == MTLStorageModePrivate || d.storageMode == MTLStorageModeShared],
                    d.swizzle);
}

// Metal's, the identity's and GM20B's refusal of a descriptor's texture: one error line per rule, and nil
static bool refusedTexture(const raw::shapes::TextureShape &s)
{
    char shape[160];
    snprintf(shape, sizeof shape,
             "%llux%llux%llu of format %u, type %u, %llu levels, %llu samples, array %llu, storage %u",
             (unsigned long long)s.width, (unsigned long long)s.height, (unsigned long long)s.depth, s.format, s.type,
             (unsigned long long)s.levels, (unsigned long long)s.samples, (unsigned long long)s.array, s.storage);
    if (const char *why = raw::shapes::metalRefusal(s)) {
        SWITCH_REFUSE_DETAIL("raw", "tex.metal", uintptr_t(why),
                             "no %s texture: nil, as Metal makes none (%s; MTLDevice.h newTextureWithDescriptor:, "
                             "nullable)",
                             shape, why);
        return true;
    }
    if (const char *why = raw::shapes::identityRefusal(s, raw::answers::kThisKind)) {
        SWITCH_REFUSE_DETAIL("raw", "tex.identity", uintptr_t(why),
                             "no %s texture: nil, as the identity's Metal makes none (%s; MTLDevice.h "
                             "newTextureWithDescriptor:, nullable)",
                             shape, why);
        return true;
    }
    if (const char *why = raw::shapes::hardwareRefusal(s)) {
        SWITCH_REFUSE_DETAIL("raw", "tex.gm20b", uintptr_t(why),
                             "no %s texture: nil (%s; MTLDevice.h newTextureWithDescriptor:, nullable)", shape, why);
        return true;
    }
    return false;
}

// The first texture of a kind the raw path holds in memory of its own, once per process
static void announceTexture(raw::shapes::Route route, const raw::shapes::TextureShape &s, const raw::Texture &t)
{
    using Route = raw::shapes::Route;
    if (route == Route::Chain) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            os_log(rawLog(), "raw: mip chain on (%llu levels of %llux%llu, %llu bytes)", (unsigned long long)s.levels,
                   (unsigned long long)s.width, (unsigned long long)s.height,
                   (unsigned long long)t.chain->layout.bytes);
    } else if (route == Route::StoredSamples) {
        const bool zeta = raw::shapes::storedSamples(s) == raw::shapes::StoredSamples::Zeta;
        static std::atomic<bool> said[2];
        if (!said[zeta].exchange(true))
            os_log(rawLog(),
                   "raw: stored %llux multisampled %{public}s on (first %llux%llu format %u: %llu bytes of %{public}s)",
                   (unsigned long long)s.samples, zeta ? "depth/stencil" : "colour", (unsigned long long)s.width,
                   (unsigned long long)s.height, s.format, (unsigned long long)(zeta ? t.storage.bytes : t.span.bytes),
                   zeta ? "zeta memory" : "block-linear memory");
    } else if (route == Route::StoredZeta) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            os_log(rawLog(),
                   "raw: stored one-sample depth/stencil on (first %llux%llu format %u, %u layers: %llu bytes of "
                   "%{public}s zeta memory)",
                   (unsigned long long)s.width, (unsigned long long)s.height, s.format, unsigned(t.layers),
                   (unsigned long long)t.storage.bytes, t.comptags ? "compressed" : "plain");
    }
}

- (id<MTLTexture>)rmNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled
{
    if (refuseTurnedOffSurface(d))
        return nil;
    using Route = raw::shapes::Route;
    raw::shapes::TextureShape s = textureShape(d);
    s.ownMemory = true;
    if (refusedTexture(s))
        return nil;
    // X32_Stencil8: Depth32Float_Stencil8's memory read through its stencil alone (NIL's X32_S8X24_UINT: the combined
    // format's header, its stencil in green), a view of a texture of the combined format it keeps
    if (d.pixelFormat == MTLPixelFormatX32_Stencil8) {
        MTLTextureDescriptor *combined = [d copy];
        combined.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        RMTexture *memory = (RMTexture *)[self rmNewTexture:combined tiled:tiled];
        return memory ? [(id<MTLTexture>)memory newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8] : nil;
    }
    // a texture buffer of the device's: its texels in a buffer of its own, under a one-dimensional buffer header (TIC
    // ONE_D_BUFFER, at most 2^27 texels: Device::createTexelBuffer)
    if (const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        f && d.textureType == MTLTextureTypeTextureBuffer) {
        const uint64_t bytes = raw::texelBufferBytes(uint32_t(d.pixelFormat), d.width, f->bpp);
        id<MTLBuffer> memory = [self newBufferWithLength:NSUInteger(bytes) options:d.resourceOptions];
        return memory ? [memory newTextureWithDescriptor:d offset:0 bytesPerRow:d.width * f->bpp] : nil;
    }
    // every shape Metal, the identity and GM20B make has a creator (raw_texture_routes_test walks them all): none is the
    // driver's gap, never the client's
    const Route route = raw::shapes::route(s);
    if (route == Route::None) {
        os_log_fault(rawLog(), "raw: %{public}s: no creator for a %lux%lu texture of format %lu, type %lu: nil",
                     getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                     (unsigned long)d.textureType);
        return nil;
    }
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = route == Route::Memoryless ? RMTextureMemoryless : RMTexturePool;
    const uint32_t kind = tiled || route != Route::Plain ? privateCompressedKind(d) : 0;
    t->_rawOk = raw::shapes::create(*gRM->raw, s, route, tiled, kind, t->_raw);
    if (t->_rawOk)
        announceTexture(route, s, t->_raw);
    else if (route == Route::StoredZeta)
        SWITCH_REFUSE_DETAIL("raw", "zeta.owned", d.pixelFormat,
                             "a stored %lux%lu depth/stencil texture of format %lu: no zeta memory or texture header "
                             "for it",
                             (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
    if (t->_rawOk && route != Route::Memoryless && !keepExtendedRange(d, t->_raw, false)) {
        gRM->raw->destroyTexture(t->_raw);
        t->_rawOk = false;
    }
    if (!t->_rawOk) {
        os_log_error(rawLog(), "raw: %{public}s: no %{public}s for a %lux%lux%lu format %lu texture: nil",
                     getprogname(), gRM->raw->headersLeft() ? "memory" : "texture header (every one is held)",
                     (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth,
                     (unsigned long)d.pixelFormat);
        return nil;
    }
    if (route != Route::Block)
        establishFresh(t, d);
    return (id<MTLTexture>)t;
}

@end
