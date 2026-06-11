#pragma once

// Pre-allocated PM value slab for the competitor fixtures (Dash / CCEH).
//
// WHY THIS EXISTS — eliminating the value-store artifact (see HIOM.md §7 / the
// priority-#2 fix). Viper / HiOM store the 200 B value by appending it into a
// pre-allocated VPage slot and persisting with a plain clwb+sfence
// (viper::internal::pmem_persist) — no allocator, no PMDK transaction. The
// Dash / CCEH fixtures originally stored every value via a per-op
// `pmem::obj::transaction::run` + `make_persistent<Entry>` (PMDK undo log +
// allocator global lock + commit fences). That made their *write* throughput
// transaction-bound and their *read* tail under write load explode — both
// artifacts of the harness's value allocator, NOT of the Dash / CCEH index.
//
// This slab gives those fixtures the SAME value-store discipline as Viper: a
// fixed-size slot in a pre-mmap'd PM region, bump-allocated, written once with
// clwb+sfence, addressed by a slot index. With it, the only thing E2/E4 measure
// that differs across the four systems is the index itself.
//
// Crash-consistency parity: this mirrors Viper's VPage value store, which is
// likewise non-transactional (write-then-mark ordering + a recovery scan, not
// PMDK undo logging). E2/E4 measure steady-state throughput/latency, not
// recovery, so the value store's recovery path is out of scope here; the point
// is to hold the value-write cost identical across systems.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#include "viper/viper.hpp"  // viper::internal::pmem_persist

namespace viper::kv_bm {

// `Entry` is the fixed-size (key, value) pair stored out-of-index, exactly the
// struct the fixtures used to `make_persistent` into a PMDK pool.
template <typename Entry>
class PmemValueSlab {
  public:
    PmemValueSlab() = default;
    PmemValueSlab(const PmemValueSlab&) = delete;
    PmemValueSlab& operator=(const PmemValueSlab&) = delete;
    ~PmemValueSlab() { destroy(); }

    // Create a fresh slab of `total_bytes` at `path` (an FS-DAX file). Bump
    // allocation starts at slot 0. FS-DAX fully allocates the file on create,
    // matching the prior PMDK pool's footprint.
    void create(const std::string& path, std::uint64_t total_bytes) {
        destroy();
        path_ = path;
        stride_ = sizeof(Entry);
        total_bytes_ = total_bytes;
        capacity_slots_ = stride_ ? (total_bytes_ / stride_) : 0;
        const int fd = ::open(path_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0)
            throw std::runtime_error("PmemValueSlab: open failed: " + path_);
        if (::ftruncate(fd, static_cast<off_t>(total_bytes_)) != 0) {
            ::close(fd);
            throw std::runtime_error("PmemValueSlab: ftruncate failed: " + path_);
        }
        base_ = static_cast<char*>(::mmap(nullptr, total_bytes_,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0));
        ::close(fd);
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            throw std::runtime_error("PmemValueSlab: mmap failed: " + path_);
        }
        next_.store(0, std::memory_order_relaxed);
    }

    void destroy() {
        if (base_) {
            ::munmap(base_, total_bytes_);
            base_ = nullptr;
        }
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            path_.clear();
        }
    }

    // Append a value: claim the next slot (atomic bump), copy the entry, and
    // persist exactly sizeof(Entry) with clwb+sfence — the SAME flush Viper
    // issues for a VPage entry (viper::internal::pmem_persist). Thread-safe:
    // distinct callers get distinct slots and write disjoint memory. Returns
    // the slot index.
    std::uint64_t put(const Entry& e) {
        const std::uint64_t slot = next_.fetch_add(1, std::memory_order_relaxed);
        if (slot >= capacity_slots_)
            throw std::runtime_error("PmemValueSlab: capacity exhausted");
        Entry* p = at(slot);
        std::memcpy(p, &e, sizeof(Entry));
        viper::internal::pmem_persist(p, sizeof(Entry));
        return slot;
    }

    // Slot accessor. The pointer is stable for the life of the slab, so the
    // index may store either the slot number or this raw pointer.
    Entry* at(std::uint64_t slot) const {
        return reinterpret_cast<Entry*>(base_ + slot * stride_);
    }

    std::uint64_t size() const { return next_.load(std::memory_order_relaxed); }

  private:
    std::string path_;
    char* base_{nullptr};
    std::uint64_t total_bytes_{0};
    std::uint64_t stride_{0};
    std::uint64_t capacity_slots_{0};
    std::atomic<std::uint64_t> next_{0};
};

}  // namespace viper::kv_bm
