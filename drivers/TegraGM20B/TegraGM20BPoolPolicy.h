// SPDX-License-Identifier: MIT
// Where a coherent BO's memory comes from (AGXDeviceUserClient::BoAlloc), as pure functions: the kernel calls them with
// the pool's live numbers, test/gm20b_pool_policy_test.cpp with a model of the pool.
//
// The GPU pool (256 MiB of write-combined memory outside the VM) serves coherent BOs (NVK's HOST_COHERENT memory: its
// event heap and upload chunks, Metal's shared storage through m2v, the raw path's write-combined chunks), and nvkm's own
// page tables and channels, unpoliced. Its tiers keep room for the compositor and nvkm. A BO the tiers keep out of the pool,
// or one the pool has no room for, is not refused (VectorKit aborts on a nil newBuffer): it is served from ordinary
// pages, wired and zeroed, mapped write-combined into the task as a pool BO is (CONTRACT §3 (b): its CPU writes are in
// memory once the DSB before GP_PUT drains them). Only a failure of those pages too (the machine out of memory)
// refuses it.
#pragma once
#include <stdint.h>
#include "SwitchGpuPoolAllocation.h"

namespace tgm_pool
{
struct Limits {
	uint64_t
		smallBytes;  // a BO up to this size only keeps floorBytes free (what a device needs to exist)
	uint64_t
		floorBytes;  // the pool's last bytes: the compositor's next m2v block and nvkm's tables for new clients
	uint64_t reserveBytes;  // a larger BO also leaves this free (kPoolReserveBytes)
	uint64_t
		clientBytes;  // and keeps its client within this much pool (kClientPoolBytes): a fairness share
};

// Why a BO of `bytes` stays out of the pool while its client holds `held` and the pool has `avail` free, or nullptr (the
// tiers). The compositor keeps only the floor (nvkm's page tables and channels, which have no other memory): with its BOs
// served from system memory past that, it is never starved, and nvkm never is either.
inline const char *refusal(uint64_t bytes, uint64_t held, uint64_t avail, const Limits &l,
			   bool compositor = false)
{
	if (avail < bytes + l.floorBytes) {
		return compositor ? "the pool's last 16 MiB stay for nvkm"
				  : "the pool's last 16 MiB stay for the compositor and nvkm";
	}
	if (compositor)
		return nullptr;
	if (bytes <= l.smallBytes)
		return nullptr;
	if (avail < bytes + l.reserveBytes)
		return "the rest stays for the compositor";
	if (held + bytes > l.clientBytes)
		return "past the client's share";
	return nullptr;
}

// Why a compressible BO of `bytes` (pool chunks, TegraGM20BBoCompressible) is refused, or nullptr. Compressed memory
// has no other source, so it leaves the floor to nvkm, whose page tables and channels have none either. The reserve
// is the compositor's room ("the rest stays for the compositor"): another client's compressed memory leaves it too
// and stays within its share, the compositor's takes it, as its coherent BOs do (past the floor they come from system
// memory). Boot 21: backboardd's 12 MiB colour target was refused at 55 MiB free, holding 60 MiB, under the old
// 48 MiB. A refusal is no failure: the target renders uncompressed and its client asks again later.
inline const char *compressibleRefusal(uint64_t bytes, uint64_t held, uint64_t avail,
				       const Limits &l, bool compositor)
{
	if (compositor)
		return avail < bytes + l.floorBytes ? "the pool's last 16 MiB stay for nvkm" : nullptr;
	if (avail < bytes + l.floorBytes + l.reserveBytes)
		return "the pool's last 48 MiB stay for the compositor and nvkm";
	if (held + bytes > l.clientBytes)
		return "past the client's share";
	return nullptr;
}

constexpr uint64_t ControlClientBytes = 16ULL << 20;
constexpr uint64_t ControlReadyBytes = SWITCH_GPU_POOL_CONTROL_READY_BYTES;
constexpr uint64_t KernelReadyBytes = 4ULL << 20;

inline bool controlReserve(bool requested, uint64_t held, uint64_t bytes)
{
	return requested && held <= ControlClientBytes && bytes <= ControlClientBytes - held;
}

enum Source : uint8_t {
	FromPool,    // the GPU pool
	FromSystem,  // ordinary pages, mapped write-combined
};

// A coherent BO's source: the pool when the tiers let the client have it (the compositor always) and the pool has it,
// else ordinary pages. `poolHasIt`: the pool allocation (after zeroing the freed ranges not zeroed yet) succeeded.
inline Source coherentSource(const char *refused, bool poolHasIt)
{
	return !refused && poolHasIt ? FromPool : FromSystem;
}
}  // namespace tgm_pool
