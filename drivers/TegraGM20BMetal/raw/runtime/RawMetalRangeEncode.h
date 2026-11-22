// SPDX-License-Identifier: MIT
// A raw range of a command buffer's tape onto the raw path in three steps: open (the raw command buffer, the range's first
// contents and resources, the planar images its draws sample), step (its commands up to a position, as many times as it
// is given more) and seal (its encoders ended, the tail, the kernel's job). Only the thread that owns the device's
// encoding (the submit worker) runs it.
#pragma once
#import "RawMetalInternal.h"
#import "RawMetalCounters.h"
#import "RawMetalFresh.h"
#import "RawMetalStandIns.h"
#include <array>
#include <deque>
#include <optional>
#include <set>
#include <vector>
#include "raw_compute.h"

class PlanDraws;
#include "raw_kernel_shadow.h"
#include "raw_pass_fusion.h"
#include "raw_reach.h"
#include "raw_turned_pass.h"
#include "raw_xr_parts.h"

class RMRangeEncode {
public:
    // `whole`: [from, to) is the whole range, its resources and first contents taken at once, after its commit. Else it is
    // encoded ahead, before the commit, as its caller records more: each encoder's resources and first contents are taken
    // as it is stepped into, nothing reads what the CPU may write until the commit (buffer contents, a surface's seed),
    // and an encoder whose encode would read such a thing stops it (vetoed()). False: *why; nothing of it was submitted.
    bool open(RMCommandBuffer *cb, size_t from, size_t to, bool whole, const char **why);
    // The range's commands up to `until`; false: failed (*why) or, encoded ahead, stopped (vetoed())
    bool step(size_t until);
    // An encode opened ahead goes on after its command buffer's commit, up to `to` where its range ends; false: as open()
    bool resume(size_t to, const char **why);
    // False: *why
    bool seal(size_t to);
    // What open() took that no seal() submitted goes back (the raw command buffer abandoned where it is still open)
    void reset();

    size_t position() const
    {
        return at_;
    }

    // Why an encode ahead stopped (RMPipelineVeto), 0 where it did not
    uint32_t vetoed() const
    {
        return veto_;
    }

    // open() or resume() stopped on an event producer still pending on a channel made again since: the range waits for its
    // outcome, which wakes its queue (raw_event_timeline.h consumers)
    bool producerPending() const
    {
        return producerPending_;
    }

    // The channel it began on (raw::Device::channelGeneration)
    uint64_t generation() const
    {
        return generation_;
    }

private:
    struct Resolve {
        uint32_t attachment;
        __unsafe_unretained RMTexture *into;
        bool keepSamples;  // MTLStoreActionStoreAndMultisampleResolve
        uint32_t level, slice;
    };

    // A depth attachment's resolve (raw_samples.h depthResolveState): a pass of the depth resolve state of its format and
    // filter into the resolve texture, writing each pixel's depth whatever it held
    struct DepthResolve {
        __unsafe_unretained RMTexture *into = nil;
        uint32_t filter = 0;
        uint32_t level = 0, slice = 0;
    };

    struct StencilResolve {  // CommandBuffer::stencilResolve's pass (raw_stencil_bits.h)
        __unsafe_unretained RMTexture *into = nil;
        raw::StencilSample pick = raw::StencilSample::Zero;
        uint32_t level = 0, slice = 0;
    };

    struct Conversion {
        __unsafe_unretained RMTexture *t;
        uint32_t seed;
        uint64_t writes, nativeGeneration;
    };

    bool fail(RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0, const char *detail = nullptr);
    bool passRefused(uint64_t a);
    bool refused();
    bool stop(uint32_t veto);
    uint32_t ineligible(size_t from, size_t until);
    bool loadsAStoredDepth(size_t x);
    bool enter(size_t begin);
    bool resources(size_t from, size_t to);
    bool borderHeaders(size_t from, size_t to);
    bool conversions();
    bool remakeImages(size_t end);
    bool makeImages(const std::vector<__unsafe_unretained RMTexture *> &planar);
    int standInPass(RMTexture *t, uint32_t slot, bool store);
    void standInBackings();
    bool storeStandIns();
    bool loadPlanes(raw::PassDesc &p);
    bool formatStandIns(const RMPass &pass, raw::PassDesc &p, const PlanDraws &draws, bool continued);
    bool storeFormatTargets();
    // The open pass's RGB10A8_2P_XR10 targets held in planes that overlap (raw_xr_pools.h xrPlanesOverlap) and plain ones
    // over rows that overlap (raw_plain_rows.h): rendered in their pools, filled first where loaded, and stored into their
    // rows after the pass
    void overlappingPlanes(const RMPass &pass, raw::PassDesc &p);
    bool storeOverlappingPlanes();
    // The open pass's targets that are texel buffers held in a pool of a block-linear surface's rows (raw_texel_rows.h):
    // rendered in their pools, filled first, copied back after the pass where it stores them (writtenBack)
    void texelRowsTargets(const RMPass &pass);
    // A blit command over such a texel buffer reads and writes its pool (blitImage): filled before the command (`before`),
    // copied back after one that writes it
    void blitTexelRows(const Cmd &c, bool before);
    // Colour j's planes `plane` .. + `layers` of `t`, laid out several planes a block (raw::planeInBlocks), as a
    // one-plane-a-block image the copy engine fills from them where `filled` and drains into them after the pass where
    // `stored` (planesBack); null: no memory for it (*why_)
    raw::Texture *planeImage(RMTexture *t, uint32_t j, uint32_t plane, uint32_t layers, bool filled, bool stored);
    void planesBack();
    void separateZeta(const RMPass &pass, RMTexture *depth, RMTexture *stencil, raw::PassDesc &p);
    bool depth16Zeta(const RMPass &pass, raw::PassDesc &p);
    bool depth16Tested(const RMPass &pass, raw::PassDesc &p);
    bool depth16Back();
    bool imagePlanes(size_t pass, raw::PassDesc &p);
    bool xrImages(const RMPass &pass, raw::PassDesc &p);
    bool writtenBack();
    bool resolvePasses();
    void refresh(size_t pass);
    bool standIns(size_t pass);
    bool drawStandIns(const std::vector<__unsafe_unretained RMTexture *> &stale);
    bool standInsGoStale(size_t encoderAt);
    void localShadows(std::vector<raw::KernelUse> &list);
    void shadowCopies(const std::vector<const void *> &back, const std::vector<const void *> &fill);
    void beginKernelShadows(size_t encoderAt);
    void endKernelShadows();
    bool command(size_t at);
    bool beginPass(size_t at);
    bool endEncoder(size_t at);
    bool endEncoders();
    bool closeEncoders();
    bool producersOrdered();

    __unsafe_unretained RMCommandBuffer *cb_ = nil;
    raw::Device *d_ = nullptr;
    raw::CommandBuffer *rc_ = nullptr;
    const char **why_ = nullptr;
    size_t from_ = 0, to_ = 0, at_ = 0;
    bool whole_ = true;
    bool ahead_ = false;  // before its commit: the recording goes on
    bool abandoned_ = false, sealed_ = false, producerPending_ = false;
    uint32_t veto_ = 0;
    bool sampledShadows_ = false, standIns_ = false;  // the command buffer's, as of its commit (ahead: none)
    uint64_t generation_ = 0;
    std::optional<FreshWrites> firstContents_;
    std::optional<RMCounterEmit> counters_;

    raw::RenderEncoder *re_ = nullptr;
    raw::BlitEncoder *be_ = nullptr;
    std::optional<raw::ComputeEncoder> ce_;
    bool serialCompute_ = false, computeUnit_ = false;
    // a predication Start or EndDoWhile naming no buffer withholds the open compute unit's dispatches
    bool unitWithheld_ = false;
    raw::KernelShadows kernelShadows_;  // the open compute encoder's (raw_kernel_shadow.h)
    std::vector<raw::KernelUse> dispatchUses_, encoderUses_;
    bool encoderShadows_ = false;
    bool encoderTracked_ = false;  // its dispatches' uses are tracked: shadowed images, or stand-ins writes reach
    raw::StateCursor stateCursor_;
    raw::FusionPass fused_;
    size_t fusedAt_ = SIZE_MAX;
    raw::PassUses planned_;
    size_t renderBegin_ = SIZE_MAX;
    uint32_t passDraws_ = 0;  // the raw command buffer's draws as the open pass began (resolvePasses: whether it drew)
    bool passDrew_ = false;   // the pass closing drew, read before the raw path's own draws after it (closeEncoders)
    bool continuing_ = false;
    size_t encoder_ = SIZE_MAX;  // the encoder being encoded (SIZE_MAX: none)
    size_t freshAt_ = SIZE_MAX;  // encoded ahead: the encoder whose first uses of fresh textures were claimed last

    // A planar render target's stand-in passes (raw_planar_target.h), with their attachment slots, and the stand-in each slot
    // took of the command buffer's for a texture that holds none
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> standInLoads_, standInStores_;
    std::array<raw::Texture *, 8> planarScratch_{};
    // overlapStores_ the open pass's targets held in planes or rows that overlap, overlapLoaded_ those of them it loads
    std::vector<__unsafe_unretained RMTexture *> planesWritten_, overlapStores_, overlapLoaded_;
    std::array<const raw::Texture *, 8> backings_{};
    // The open pass's loaded two-plane YCbCr targets and their slots, the tiles its draws reach (raw_ycbcr_load.h) and the
    // targets whose stores write those tiles alone
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> planeLoads_;
    raw::PassReach loadedReach_;
    uint8_t loadedTargets_ = 0;
    // Targets of a format the GM20B renders none of (raw_format_standin.h): the open pass's and their slots, then each
    // stored attachment's image and its stand-in, put back after the pass
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> formatTargets_;
    struct FormatStore {
        raw::ColorAttachment attachment;
        uint32_t layers;
        raw::Texture *standIn;
    };
    std::vector<FormatStore> formatStores_;
    // The open pass's plane images its stores drain into their 3D textures (planeImage)
    struct PlaneImage {
        __unsafe_unretained RMTexture *t;
        raw::Texture *image;
        uint32_t plane, layers;
    };
    std::vector<PlaneImage> planeImages_;
    std::vector<Resolve> resolves_;
    std::deque<raw::Texture> resolveTargets_;  // the encoder reads them until its end()
    raw::TurnedPass turned_;                   // the open pass's turn copies (raw_turned_pass.h)
    DepthResolve depthResolve_;
    StencilResolve stencilResolve_;
    std::vector<__unsafe_unretained RMSampler *> bordered_;
    std::vector<Conversion> converted_;  // planar images converted here, at their surfaces' seeds and write counts
    // the converted textures the range samples whose surface it also writes: drawn again after each encoder that writes
    // it, ahead of the next work that samples them (remakeImages)
    std::vector<__unsafe_unretained RMTexture *> rewritten_;
    std::set<RMTexture *> fresh_;
    raw::Texture levelTargets_[8];  // the open pass's level targets: the encoder reads them until its end()
    raw::Texture zetaPart_;         // and its depth/stencil attachment's layer
    raw::Texture separatePart_[2];  // a separate depth and stencil texture's layers from their slices
    // a separate depth or stencil texture a resolve reads: the pass stores the aspect back into it, whose own resolve state
    // reads it
    const raw::Texture *separateResolved_[2] = {};
    // a stored Depth16Unorm beside a Stencil8, rendered through a Depth32Float_Stencil8 scratch: converted back at the end
    raw::Texture *depth16Scratch_ = nullptr;
    // Textures the open pass's draws store into: through a sampled shadow, copied back after the pass; under a stand-in,
    // drawn again before later work samples it; two-plane RGB10A8_2P_XR10 ones through an image of their texels, merged
    // from the planes before the pass and split back after it (RawMetalXrPlanes.mm)
    std::vector<__unsafe_unretained RMTexture *> shadowsWritten_, standInsWritten_;
    std::vector<std::pair<__unsafe_unretained RMTexture *, raw::Texture *>> planesImaged_;
    std::vector<raw::PassDesc::WrittenPlanes> morePlanes_;  // past raw::PassDesc's inline ones (its morePlanes)
    RMStandInFresh standInFresh_;
    std::deque<raw::Texture> codes_;  // the XR conversions' sources: the codes' headers (read until the encoder's end)
    std::deque<raw::XrPart> codeParts_;  // a layered, mipmapped, 3D or multisampled one's 2D images (raw_xr_parts.h)
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t levelPasses_ = 0;
#endif
};

// The submit worker's one range encode
RMRangeEncode &rmRangeEncode();
// A texture a kernel or a draw reaches through an image of its own (raw_kernel_shadow.h): that image filled from the
// texels where `fill`, else copied back into them. Two-plane RGB10A8_2P_XR10 planes' pools (raw_xr_pools.h), a texel
// buffer's pool of its surface's rows (raw_texel_rows.h), or Texture::sampled
void rmMoveShadowImage(raw::BlitEncoder &b, const raw::Texture &t, bool fill);
// The refusal predicate of an encode-time refusal or skip (raw_gpu.h RefusalId)
RMPred refusalPred(uint8_t id);
