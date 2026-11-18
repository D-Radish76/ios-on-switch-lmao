// SPDX-License-Identifier: MIT
// The raw GM20B path as a process's Metal device (TegraGM20BMetalPlugin.mm decides: every process whose sandbox may read the pack).
//
// Every Metal object it hands out answers on the raw path (raw_gpu.h: direct MAXWELL_B methods). A command buffer is recorded as
// it is encoded and encoded on the raw path at commit; what the raw path cannot run fails it with Metal's error. A device
// selector it has no method for is answered zero where a Metal protocol declares it.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "raw_device_open.h"

#ifdef __cplusplus
extern "C" {
#endif
// The render server's: starts the raw path's own setup on a thread of its own (the pack read, the kernel connection, the channel
// and its context, the shader heap, the first pool chunk), so it runs beside the plugin's cache directory search;
// raw_metal_device_open with the same pack and key takes its result. A device opened without it is another process's
// (raw_gpu.h Device::Role::Client): each of those made at its first use, in smaller steps.
void raw_metal_prepare(const char *packPath, uint64_t stack);
// Turns on the runtime's completion of the Core Animation states the pack lacks (RawComplete.h): their records made off the render
// thread and kept under `cacheDir`, the process's own shader cache directory (null: in memory only), the installed native records
// under `nativeSeed` read first (RecordCache::attachSeed; null: none). After raw_metal_device_open.
void raw_metal_complete_states(const char *cacheDir, const char *nativeSeed = nullptr);
// The pack key of a render pipeline descriptor whose functions are this facade's (the frontend journal's R-line fields but the label,
// then " | l <framework>" for functions of a framework's default library other than QuartzCore's).
NSString *raw_metal_pipeline_key(MTLRenderPipelineDescriptor *descriptor);
// Whether every function the descriptor links is a framework library's, which the pack can name
bool raw_metal_linked_packable(MTLRenderPipelineDescriptor *descriptor);
// One rates line since the previous call (0 when the device did nothing): command buffers on each path and why, encode time,
// draws, passes, copies, waits, memory; and the states the runtime made since the device opened (states=<records from the
// cache>/<probes>/<refused>/<failed>/<ms making them>). The plugin logs it every 5 s in debug builds.
size_t raw_metal_rates(char *out, size_t size);
size_t raw_metal_split_rates(char *out, size_t size);  // RawMetalRates.mm: split twins taken and not, by why
// Debug builds: one line of the fetching draws since the previous call (0 when there were none), by how each kept raster
// order (raw_fetch_order.h FetchCounts). The plugin logs it after the rates line.
size_t raw_metal_fetch_rates(char *out, size_t size);
// Debug builds: "encode n=<encodes> wall= res= loop= ... pmax= ... walk= ... geov=" since the previous call (0 when
// nothing was encoded): the submit worker's encodes of raw ranges, in microseconds per encode by part (raw_encode_profile.h;
// a part includes those nested in it, a self time leaves them out), "/x" the calls per encode, pmax= the longest
// prepare(), then the encode's events per command buffer (mallocs, locks, sends, slots per draw).
size_t raw_metal_encode_rates(char *out, size_t size);
// "fetch keys rec=<key>:n,... enc=... sto=...": the pipeline keys behind the window's record-time and encode-time
// refusals and storage-to-barrier fallbacks, eight of each at most, most first; 0 when none
size_t raw_metal_fetch_keys(char *out, size_t size);
// Host tests: the CPU's view of the image the raw path samples for `texture` (a planar texture's converted image), its row pitch
// in *pitch; null when the raw path holds none.
const void *raw_metal_texture_image(id<MTLTexture> texture, size_t *pitch);
// Whether the raw path keeps `texture` (host tests: test/shapecheck.mm), and whether mip chains are on (the self-test at open)
bool raw_metal_texture_kept(id<MTLTexture> texture);
bool raw_metal_chains_on(void);
#ifdef __cplusplus
}
#endif

// A raw device over the pack at `packPath` (its stack key must be `stack`; 0: any, for host tests), an _MTLDevice of its
// own, answering registryID with `registryID`. Nil: the reason in *error.
id<MTLDevice> raw_metal_device_open(const char *packPath, uint64_t stack, uint64_t registryID, NSString **error);
// What the last raw_metal_device_open that returned nil met (raw_device_open.h)
raw::OpenFailure raw_metal_open_failure(void);
