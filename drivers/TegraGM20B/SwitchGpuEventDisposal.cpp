// SPDX-License-Identifier: MIT
#include "SwitchGpuEventInternal.h"
#include "SwitchGpuEventWord.h"
#include "TegraGM20BABI.h"

extern "C" int proc_selfpid(void);

namespace
{
SwitchGpuEventRecord *disposals;

// Event records, kRecords in all. A compositor's process takes any; every other task at most kTaskRecords, and
// together they leave the last kCompositorRecords. A compositor job with events holds up to TegraGM20BMaxJobEvents + 1
// records until it retires and its connection's watches up to 128: the reserve covers the watches and 27 such jobs in
// flight. Tasks are counted by pid in kBuckets buckets (two that share one share its quota); kernel threads, the VIC's
// engine thread among them (it prepares apps' jobs), count as pid 0.
constexpr uint32_t kRecords = 4096, kCompositorRecords = 1024, kTaskRecords = 512;
constexpr uint32_t kBuckets = 256, kCompositors = 16;
static_assert(kCompositorRecords >= 128 + 27 * (TegraGM20BMaxJobEvents + 1),
	      "the compositor's reserve");
static_assert(kRecords - kCompositorRecords > kTaskRecords, "one task leaves the others room");
uint32_t records, buckets[kBuckets];
uint64_t compositors[kCompositors];  // (pid << 32) | its compositor connections, 0 a free entry

bool take(uint32_t *counter, uint32_t limit)
{
	uint32_t count = __atomic_load_n(counter, __ATOMIC_RELAXED);
	while (count < limit)
		if (__atomic_compare_exchange_n(counter, &count, count + 1, false, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			return true;
	return false;
}

uint64_t *compositorEntry(uint32_t pid)
{
	for (auto &entry : compositors) {
		const uint64_t word = __atomic_load_n(&entry, __ATOMIC_ACQUIRE);
		if (word && word >> 32 == pid)
			return &entry;
	}
	return nullptr;
}

bool countCompositor(uint32_t pid)
{
	for (;;) {
		if (uint64_t *entry = compositorEntry(pid)) {
			uint64_t word = __atomic_load_n(entry, __ATOMIC_RELAXED);
			if (word >> 32 == pid && word &&
			    __atomic_compare_exchange_n(entry, &word, word + 1, false,
							__ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
				return true;
			continue;
		}
		bool free = false;
		for (auto &entry : compositors) {
			uint64_t empty = 0;
			free |= __atomic_load_n(&entry, __ATOMIC_RELAXED) == 0;
			const uint64_t first = uint64_t(pid) << 32 | 1;
			if (__atomic_compare_exchange_n(&entry, &empty, first, false,
							__ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
				return true;
		}
		if (!free)
			return false;
	}
}

void uncountCompositor(uint32_t pid)
{
	while (uint64_t *entry = compositorEntry(pid)) {
		uint64_t word = __atomic_load_n(entry, __ATOMIC_RELAXED);
		if (word >> 32 != pid || !word)
			continue;
		const uint64_t next = uint32_t(word) == 1 ? 0 : word - 1;
		if (__atomic_compare_exchange_n(entry, &word, next, false, __ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return;
	}
}
}  // namespace

bool switch_gpu_events_reserve(uint32_t *quota)
{
	const int pid = proc_selfpid();
	const bool compositor = pid > 0 && compositorEntry(uint32_t(pid));
	if (!take(&records, compositor ? kRecords : kRecords - kCompositorRecords))
		return false;
	*quota = 0;
	if (compositor)
		return true;
	const uint32_t bucket = uint32_t(pid) % kBuckets;
	if (take(&buckets[bucket], kTaskRecords)) {
		*quota = bucket + 1;
		return true;
	}
	__atomic_sub_fetch(&records, 1u, __ATOMIC_RELAXED);
	return false;
}

void switch_gpu_events_unreserve(uint32_t quota)
{
	if (quota)
		__atomic_sub_fetch(&buckets[quota - 1], 1u, __ATOMIC_RELAXED);
	__atomic_sub_fetch(&records, 1u, __ATOMIC_RELAXED);
}

extern "C" int switch_gpu_events_compositor(int pid, int on)
{
	if (pid <= 0)
		return -22;
	if (!on) {
		uncountCompositor(uint32_t(pid));
		return 0;
	}
	return countCompositor(uint32_t(pid)) ? 0 : -28;
}

void switch_gpu_event_dispose(SwitchGpuEventRecord *record)
{
	auto *head = __atomic_load_n(&disposals, __ATOMIC_RELAXED);
	do {
		record->disposeNext = head;
	} while (!__atomic_compare_exchange_n(&disposals, &head, record, false, __ATOMIC_RELEASE,
					      __ATOMIC_RELAXED));
	switch_gpu_events_schedule();
}

void switch_gpu_events_dispose()
{
	while (auto *head = __atomic_exchange_n(&disposals, nullptr, __ATOMIC_ACQUIRE)) {
		while (head) {
			auto *record = head;
			head = record->disposeNext;
			const uint32_t quota = record->quota;
			if (record->kind == SwitchEventSignal) {
				auto *signal = static_cast<switch_gpu_event_signal *>(record);
				if (signal->bound)
					switch_gpu_backing_token_release(&signal->token);
				if (signal->dependencies)
					switch_gpu_event_waits_drop(signal->dependencies);
				switch_gpu_event_word_release(&signal->word);
				IODelete(signal, switch_gpu_event_signal, 1);
			} else if (record->kind == SwitchEventUse) {
				auto *use = static_cast<switch_gpu_event_use *>(record);
				switch_gpu_backing_token_release(&use->token);
				if (use->authority)
					switch_gpu_event_signal_drop(use->authority);
				else if (use->batchAuthority)
					switch_gpu_event_waits_drop(use->batchAuthority);
				IODelete(use, switch_gpu_event_use, 1);
			} else {
				auto *batch = static_cast<switch_gpu_event_waits *>(record);
				for (uint32_t i = 0; i < batch->count; ++i)
					switch_gpu_event_use_release(&batch->uses[i]);
				if (batch->bound)
					switch_gpu_backing_token_release(&batch->consumer);
				if (batch->previous)
					switch_gpu_event_waits_drop(batch->previous);
				if (batch->uses && batch->uses != batch->inlineUses)
					IODelete(batch->uses, switch_gpu_event_use *,
						 batch->capacity);
				IODelete(batch, switch_gpu_event_waits, 1);
			}
			switch_gpu_events_unreserve(quota);
		}
	}
}
