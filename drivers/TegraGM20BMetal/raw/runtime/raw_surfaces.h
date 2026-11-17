// SPDX-License-Identifier: MIT
// Exact backing uses for one job.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "TegraGM20BABI.h"

namespace raw {
enum class JobVisibility : uint8_t {
    Legacy,
    Internal,
    External
};

class SurfaceList {
public:
    void clear()
    {
        uses_.clear();
        ranges_.clear();
        extents_.clear();
        if (largeIndex_.empty())
            index_.fill(0);
        else
            std::fill(largeIndex_.begin(), largeIndex_.end(), 0);
        overflow_ = false;
        last_ = 0;
    }

    void add(uint64_t handle, bool write)
    {
        addAccess(handle, write ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
    }

    void addAddress(uint64_t va, uint64_t bytes, bool write, uint32_t metadata = 0)
    {
        if (!bytes || bytes > UINT64_MAX - va || ((metadata & TegraGM20BSurfaceRange) && bytes > UINT32_MAX)) {
            overflow_ = true;
            return;
        }
        const uint32_t access =
            TegraGM20BSurfaceGpuAddress | (write ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead) | metadata;
        const size_t slot = findSlot(va, TegraGM20BSurfaceGpuAddress);
        if (indexAt(slot)) {
            last_ = indexAt(slot) - 1;
            const uint64_t previous = extents_[last_];
            mergeAddress(last_, access, bytes);
            if (bytes <= previous)
                return;
            extents_[last_] = bytes;
            if (bytes > 1)
                ranges_.push_back({va, va + bytes, last_});
            return;
        }
        for (const auto &range : ranges_)
            if (va >= range.begin && va + bytes <= range.end) {
                mergeAddress(range.index, access, extents_[range.index]);
                return;
            }
        addAccess(va, access);
        if (valid()) {
            mergeAddress(last_, access, bytes);
            if (bytes > 1)
                ranges_.push_back({va, va + bytes, last_});
        }
    }

    void addAccess(uint64_t handle, uint32_t access)
    {
        if (!handle) {
            overflow_ = true;
            return;
        }
        const uint32_t kind = access & TegraGM20BSurfaceGpuAddress;
        if (last_ < uses_.size() && uses_[last_].handle == handle &&
            (uses_[last_].access & TegraGM20BSurfaceGpuAddress) == kind) {
            if ((access & TegraGM20BSurfaceWrite) && !(access & TegraGM20BSurfaceRange)) {
                uses_[last_].access &= ~uint32_t(TegraGM20BSurfaceRange);
                uses_[last_].reserved = 0;
            }
            uses_[last_].access |= access;
            return;
        }
        size_t slot = findSlot(handle, kind);
        if (indexAt(slot)) {
            last_ = indexAt(slot) - 1;
            if ((access & TegraGM20BSurfaceWrite) && !(access & TegraGM20BSurfaceRange)) {
                uses_[last_].access &= ~uint32_t(TegraGM20BSurfaceRange);
                uses_[last_].reserved = 0;
            }
            uses_[last_].access |= access;
            return;
        }
        if (uses_.size() == UINT32_MAX) {
            overflow_ = true;
            return;
        }
        if (uses_.size() >= indexSize() / 2) {
            largeIndex_.assign(indexSize() * 2, 0);
            for (size_t i = 0; i < uses_.size(); ++i)
                largeIndex_[findSlot(uses_[i].handle, uses_[i].access & TegraGM20BSurfaceGpuAddress)] = uint32_t(i + 1);
            slot = findSlot(handle, kind);
        }
        last_ = uses_.size();
        uses_.push_back({handle, access, 0});
        extents_.push_back(0);
        if (largeIndex_.empty())
            index_[slot] = uint16_t(last_ + 1);
        else
            largeIndex_[slot] = uint32_t(last_ + 1);
    }

    bool valid() const
    {
        return !overflow_;
    }

    const std::vector<TegraGM20BAbiSurfaceUse> &uses() const
    {
        return uses_;
    }

    // Ownership is per backing, including every pooled suballocation. A CPU geometry reader must not coexist
    // with a write in the same job. Physical aliases under another handle are additionally checked by the kernel.
    bool writesBacking(uint64_t handle, uint64_t va, uint64_t bytes) const
    {
        if (!valid())
            return true;
        for (const auto &use : uses_) {
            if (!(use.access & TegraGM20BSurfaceWrite))
                continue;
            if (use.access & TegraGM20BSurfaceGpuAddress) {
                if (use.handle >= va && use.handle - va < bytes)
                    return true;
            } else if (use.handle == handle) {
                return true;
            }
        }
        return false;
    }

private:
    void mergeAddress(size_t index, uint32_t access, uint64_t bytes)
    {
        auto &use = uses_[index];
        const bool oldWrite = (use.access & TegraGM20BSurfaceWrite) != 0;
        const bool addedWrite = (access & TegraGM20BSurfaceWrite) != 0;
        const bool oldExact = (use.access & TegraGM20BSurfaceRange) != 0;
        const bool addedExact = (access & TegraGM20BSurfaceRange) != 0;
        const bool exact = addedWrite ? addedExact && (!oldWrite || oldExact)
                           : oldWrite ? oldExact
                                      : oldExact || addedExact;
        extents_[index] = std::max(extents_[index], bytes);
        use.access = (use.access | access) & ~uint32_t(TegraGM20BSurfaceRange);
        use.reserved = 0;
        if (exact) {
            if (extents_[index] > UINT32_MAX) {
                overflow_ = true;
                return;
            }
            use.access |= TegraGM20BSurfaceRange;
            use.reserved = uint32_t(extents_[index]);
        }
    }

    size_t findSlot(uint64_t handle, uint32_t kind) const
    {
        uint64_t hash = handle ^ (uint64_t(kind) << 61);
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
        hash *= 0xc4ceb9fe1a85ec53ULL;
        hash ^= hash >> 33;
        size_t slot = hash & (indexSize() - 1);
        while (indexAt(slot)) {
            const auto &use = uses_[indexAt(slot) - 1];
            if (use.handle == handle && (use.access & TegraGM20BSurfaceGpuAddress) == kind)
                break;
            slot = (slot + 1) & (indexSize() - 1);
        }
        return slot;
    }

    struct Range {
        uint64_t begin, end;
        size_t index;
    };

    size_t indexSize() const
    {
        return largeIndex_.empty() ? index_.size() : largeIndex_.size();
    }

    uint32_t indexAt(size_t slot) const
    {
        return largeIndex_.empty() ? index_[slot] : largeIndex_[slot];
    }

    std::array<uint16_t, 2048> index_{};
    std::vector<uint32_t> largeIndex_;
    std::vector<uint64_t> extents_;
    std::vector<Range> ranges_;
    std::vector<TegraGM20BAbiSurfaceUse> uses_;
    size_t last_ = 0;
    bool overflow_ = false;
};
}  // namespace raw
