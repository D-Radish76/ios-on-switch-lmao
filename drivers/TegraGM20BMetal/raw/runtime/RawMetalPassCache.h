// SPDX-License-Identifier: MIT
// The passes a recording opened, by their descriptors' contents: a render pass descriptor of Metal's own classes is
// read as the bytes of its objects (their ivars found by name once, raw_objc_layout.h), with no message and nothing it
// would make lazily, and a pass of the same contents as one opened before takes that pass's copy of its descriptor and the
// facts read from it (Core Animation opens its passes of a frame in the same few shapes each frame). The copy is shared and
// read-only: an encoder that changes its pass (a store action it decides, a parallel encoder's seam) takes its own. The raw
// encode reads a pass's RMPass alone, so a found copy names no texture until m2v's replay is to read it (rmPassBind), and a
// copy that names any lets them go once its command buffer is done: the driver keeps no client object alive past it, as
// Metal keeps none, and a key names each by its identity (RMObject::_identity), never by an address a new one may take.
#pragma once
#import "RawMetalInternal.h"
#include <array>
#include <cstdint>
#include <vector>

// What a hit hands back: the copy, its facts, and what the encoder took from them
struct RMPassEntry {
    __unsafe_unretained MTLRenderPassDescriptor *pass;  // retained by the cache (CFBridgingRetain), not by ARC
    __unsafe_unretained id visibility;                  // its visibility result buffer
    __unsafe_unretained id zetaResolve[2];              // its depth's and stencil's resolve textures, resolving or not
    RMPass facts;
    std::array<uint32_t, 8> formats;
    uint32_t count, zeta, samples;
    uint8_t turn;
    std::vector<uint8_t> key;
};

// `d`'s contents, as the key a find and a keep take: false where `d` is not of Metal's own descriptor classes, a sample
// buffer attachment names a buffer (counter marks read and clear them on the pass's copy), or it names an object that is no
// texture or buffer of this device
bool rmPassKey(MTLRenderPassDescriptor *d, std::vector<uint8_t> &key, uint64_t &hash);
// A command buffer owns its bank from rmTakeRecording to rmRecordingDone: no trim touches an entry it found.
RMPassEntry *rmPassFind(RMCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash);
// The pass just opened from `copy`, kept for the next one of the same contents
void rmPassKeep(RMCommandBuffer *cb, const std::vector<uint8_t> &key, uint64_t hash, MTLRenderPassDescriptor *copy,
                const RMPass &facts, const RMRenderEncoder *e);

struct RMPassTrim {
    size_t passes, textures;
};

// The kept recordings' passes let go (pressure and idle release, and a depth image's move, which their source headers name)
RMPassTrim rmPassTrim();
// What the kept recordings' banks hold now (debug builds' rates line).
RMPassTrim rmPassHeld();

RMPassCachePtr rmNewPassCache();
// Its command buffer is done: every copy it named textures in names none
void rmPassCacheUnbind(RMPassCache *cache);
// The kept copy `pass` names its pass's textures for m2v's replay of `cb`, once a command buffer; [from, to): those of the
// range's passes
void rmPassBind(RMCommandBuffer *cb, MTLRenderPassDescriptor *pass);
void rmPassBind(RMCommandBuffer *cb, size_t from, size_t to);
RMPassTrim rmPassCacheTrim(RMPassCache *cache);
RMPassTrim rmPassCacheHeld(const RMPassCache *cache);
size_t rmPassCacheBytes(const RMPassCache *cache);
