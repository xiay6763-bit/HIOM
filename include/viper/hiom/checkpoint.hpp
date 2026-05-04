#pragma once

// HiOM A/B checkpoint protocol (M5). Tiny PM file (4 KB) holding two
// alternating CheckpointRecord slots and an atomic valid_pointer.
//
// Design (paper §5):
//   - Two slots in PM, only one "valid" at any time.
//   - 8-byte atomic valid_pointer selects which.
//   - Write protocol: write fresh record into the inactive slot,
//     pmem_persist, then atomic-store the new valid_pointer.
//   - Read protocol: load valid_pointer, read that slot, verify
//     summary_hash; on mismatch (torn write), fall back to the
//     other slot.
//
// The point of having two slots is to keep at most one mid-write at
// any time. A crash during slot write can torn-tear the in-progress
// slot, but the previous slot stays intact and valid_pointer hasn't
// flipped yet — so recovery sees the older but consistent record.
//
// What goes into a record (M5 minimum):
//   - magic         : identifies this as a HiOM checkpoint record
//   - seq           : monotonic; higher always wins on read
//   - flushed_count : running total of buffer entries that have
//                     reached ColdTier (HiOM stat)
//   - vpage_frontier: snapshot of Viper::current_block_page_; M6
//                     will use this to bound the recovery VPage scan
//   - cold_size     : ColdTier::approx_size() at checkpoint time
//   - summary_hash  : CRC of the rest, defends against torn slot writes
//
// Why a separate PM file (not piggy-back on ColdTier's
// GlobalHeader.reserved): Checkpoint is its own self-contained,
// independently testable unit; ColdTier's pool file is one large
// mmap'd region and cross-module reads of its header would couple
// modules unnecessarily. The cost of a 4 KB extra PM file is zero.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "viper/viper.hpp"  // viper::internal::pmem_persist

namespace viper::hiom {

struct CheckpointRecord {
    static constexpr std::uint64_t kMagic = 0x484f4d5f43485054ull;  // "HOM_CHPT"

    std::uint64_t magic{0};
    std::uint64_t seq{0};
    std::uint64_t flushed_count{0};
    std::uint64_t vpage_frontier{0};
    std::uint64_t cold_size{0};
    std::uint64_t reserved[2]{0, 0};
    std::uint64_t summary_hash{0};
};
static_assert(sizeof(CheckpointRecord) == 64,
              "CheckpointRecord must fit a single cache line");

class Checkpoint {
  public:
    static constexpr std::uint64_t kFileSize = 4096;
    static constexpr std::uint64_t kSlotASize = 128;  // padded room
    static constexpr std::uint64_t kSlotBOffset = 64 + kSlotASize;
    static constexpr std::uint64_t kSlotAOffset = 64;
    static constexpr std::uint64_t kValidNone = static_cast<std::uint64_t>(-1);

    // Atomic valid_pointer values: 0 selects slot A, 1 selects slot B,
    // kValidNone means no checkpoint has been written yet.
    static constexpr std::uint64_t kSlotA = 0;
    static constexpr std::uint64_t kSlotB = 1;

    static std::unique_ptr<Checkpoint> create(const std::string& pool_file) {
        const int fd = ::open(pool_file.c_str(),
                              O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) throw std::runtime_error(
            "Checkpoint::create: open failed: " + pool_file);
        if (::ftruncate(fd, kFileSize) != 0) {
            ::close(fd);
            throw std::runtime_error("Checkpoint::create: ftruncate failed");
        }
        void* base = ::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED) throw std::runtime_error(
            "Checkpoint::create: mmap failed");

        std::memset(base, 0, kFileSize);
        // valid_pointer = -1 means "no checkpoint yet". Persist the
        // initial state so a crash before the first write() leaves
        // open() observing kValidNone (and read_valid returning
        // nullopt) rather than reading garbage.
        auto* vp = static_cast<std::atomic<std::uint64_t>*>(base);
        vp->store(kValidNone, std::memory_order_release);
        viper::internal::pmem_persist(base, sizeof(std::uint64_t));

        return std::unique_ptr<Checkpoint>(
            new Checkpoint(pool_file, base));
    }

    static std::unique_ptr<Checkpoint> open(const std::string& pool_file) {
        const int fd = ::open(pool_file.c_str(), O_RDWR, 0644);
        if (fd < 0) throw std::runtime_error(
            "Checkpoint::open: open failed: " + pool_file);
        struct stat st;
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("Checkpoint::open: fstat failed");
        }
        if (static_cast<std::uint64_t>(st.st_size) != kFileSize) {
            ::close(fd);
            throw std::runtime_error("Checkpoint::open: unexpected file size");
        }
        void* base = ::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED) throw std::runtime_error(
            "Checkpoint::open: mmap failed");
        return std::unique_ptr<Checkpoint>(
            new Checkpoint(pool_file, base));
    }

    ~Checkpoint() {
        if (base_) ::munmap(base_, kFileSize);
    }
    Checkpoint(const Checkpoint&) = delete;
    Checkpoint& operator=(const Checkpoint&) = delete;

    // Write `rec` into the inactive slot, persist, then atomic-flip
    // valid_pointer. seq is taken from `rec.seq` — caller is
    // responsible for monotonic sequencing. magic and summary_hash
    // are auto-filled.
    void write(CheckpointRecord rec) {
        rec.magic = CheckpointRecord::kMagic;
        rec.summary_hash = compute_hash(rec);

        const std::uint64_t cur = valid_pointer_atomic().load(
            std::memory_order_acquire);
        // Inactive slot is the one valid_pointer is NOT pointing at.
        // If no checkpoint yet (kValidNone), write to slot A first.
        const std::uint64_t target_slot
            = (cur == kSlotA) ? kSlotB : kSlotA;
        const std::uint64_t target_offset
            = (target_slot == kSlotA) ? kSlotAOffset : kSlotBOffset;

        auto* slot_ptr = reinterpret_cast<CheckpointRecord*>(
            static_cast<char*>(base_) + target_offset);
        std::memcpy(slot_ptr, &rec, sizeof(rec));
        viper::internal::pmem_persist(slot_ptr, sizeof(rec));

        // Atomic flip. 8-byte atomic store on Optane ADR is durable
        // by itself, but we still issue clwb+sfence for ordering with
        // subsequent reads from other threads / processes.
        valid_pointer_atomic().store(target_slot, std::memory_order_release);
        viper::internal::pmem_persist(base_, sizeof(std::uint64_t));
    }

    // Read the currently valid checkpoint, or std::nullopt if no
    // checkpoint has been written yet (or both slots fail their
    // summary_hash, indicating torn writes — in that case we'd want
    // to escalate to a recovery scan, but M5 just reports nullopt).
    std::optional<CheckpointRecord> read_valid() const {
        const std::uint64_t vp = valid_pointer_atomic().load(
            std::memory_order_acquire);
        if (vp == kValidNone) return std::nullopt;
        if (vp != kSlotA && vp != kSlotB) return std::nullopt;

        // Try the indicated slot first; if its hash fails, try the
        // other (a fresh torn write reverted to the prior good slot).
        if (auto rec = read_slot(vp)) return rec;
        const std::uint64_t fallback = (vp == kSlotA) ? kSlotB : kSlotA;
        return read_slot(fallback);
    }

    // Diagnostics — surface raw slot contents for tests.
    std::pair<std::optional<CheckpointRecord>,
              std::optional<CheckpointRecord>>
    read_both() const {
        return {read_slot(kSlotA), read_slot(kSlotB)};
    }

    std::uint64_t valid_pointer_raw() const {
        return valid_pointer_atomic().load(std::memory_order_acquire);
    }

    const std::string& pool_file() const { return pool_file_; }

  private:
    Checkpoint(const std::string& pool_file, void* base)
        : pool_file_(pool_file), base_(base) {}

    std::atomic<std::uint64_t>& valid_pointer_atomic() const {
        return *static_cast<std::atomic<std::uint64_t>*>(base_);
    }

    std::optional<CheckpointRecord> read_slot(std::uint64_t which) const {
        const std::uint64_t off
            = (which == kSlotA) ? kSlotAOffset : kSlotBOffset;
        CheckpointRecord rec;
        std::memcpy(&rec,
                    static_cast<const char*>(base_) + off,
                    sizeof(rec));
        if (rec.magic != CheckpointRecord::kMagic) return std::nullopt;
        if (rec.summary_hash != compute_hash(rec)) return std::nullopt;
        return rec;
    }

    // Simple FNV-1a 64-bit hash over everything except the
    // summary_hash field itself. Detects torn slot writes (any byte
    // change flips the hash); not designed against intentional
    // tampering.
    static std::uint64_t compute_hash(const CheckpointRecord& rec) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&rec);
        const std::size_t hash_offset
            = offsetof(CheckpointRecord, summary_hash);
        for (std::size_t i = 0; i < hash_offset; ++i) {
            h ^= bytes[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    std::string pool_file_;
    void* base_{nullptr};
};

}  // namespace viper::hiom
