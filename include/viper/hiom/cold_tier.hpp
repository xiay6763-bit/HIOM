#pragma once

// HiOM cold tier — PM-resident hash index, eventually the authoritative
// offset map (replacing Viper's CCEH; see §3 of design/HIOM.md).
//
// Phase B-1 scope (this turn): standalone ColdTier with insert / lookup
// / delete on real PM, **overflow chains** so insert never returns false
// at <100% load, and a `parallel_load(num_threads)` entry point for
// region-parallel recovery.
//
// Still deferred to Phase B-2:
//   - Linear-hash split worker (per-region split_ptr advancement). Until
//     then, capacity is fixed at construction. Pre-size main buckets +
//     overflow pool so the workload fits comfortably.
//   - HiOM integration (replacing Viper's CCEH on miss path).
//
// Persistence model:
//   - Entry: 16 B = 8 B `fingerprint` + 8 B `offset` (full
//     viper::KeyValueOffset).
//   - Update: 8 B atomic CAS on the offset field — Optane ADR provides
//     8 B atomic durability; we still issue clwb+sfence via
//     viper::internal::pmem_persist for ordering.
//   - Insert: write entry first, persist; THEN flip occupancy bit and
//     persist header. Crash with bit unset → slot treated as free.
//     Same pattern as Viper's VPage write-then-bitset
//     (viper.hpp:1037-1054).
//   - Delete: 8 B CAS the offset to kTombstoneOffset, persist. The
//     fingerprint stays in place so re-insert finds the slot via
//     match-and-update.
//
// PM layout:
//   [0, 4096)                   GlobalHeader
//   [4096, ...)                 Region[0..31], each = RegionHeader + main_buckets
//                                                   + overflow_pool
//
// Per region (compact):
//   [16 B RegionHeader] [num_main_buckets × Bucket] [num_overflow_slots × Bucket]
//
// Per bucket (128 B = 2 cache lines):
//   header (8 B):
//     bits 0..6  occupancy_bitmap (7 entries)
//     bit  7     has_overflow flag
//     bits 8..63 next_overflow_idx (56 bits = up to 2^56 chained buckets)
//   7 × Entry (16 B each) = 112 B
//   8 B padding to 128
//
// Overflow chain: when a bucket is full of distinct fingerprints, we
// allocate a fresh bucket from the region's overflow pool (atomic
// fetch_add on `RegionHeader::next_overflow_alloc`), write the entry
// there, persist, then CAS the chain-head bucket's header to attach
// the new overflow tail. Lookup follows the chain until a match or
// chain end.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/mman.h>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "viper/viper.hpp"  // viper::internal::pmem_persist + KeyValueOffset

namespace viper::hiom {

class ColdTier {
  public:
    using Offset = viper::KeyValueOffset;

    static constexpr std::uint64_t kMagic = 0x484f4d5f434f4c44ull;  // "HOM_COLD"
    static constexpr std::uint32_t kVersion = 2;                    // bumped: layout has overflow pool
    static constexpr std::size_t kNumRegions = 32;
    static constexpr std::size_t kEntriesPerBucket = 7;
    static constexpr std::size_t kBucketSize = 128;
    static constexpr std::uint64_t kEmptyFp = 0;
    static constexpr std::uint64_t kTombstoneOffset = 0xFFFF'FFFF'FFFF'FFFFull;
    static constexpr std::uint64_t kNoOverflow = 0;  // 0 means "none" (idx 0 reserved)

    static constexpr std::size_t kDefaultMainBuckets = 8192;
    static constexpr std::size_t kDefaultOverflowSlots = 16384;  // 2x main

    struct Entry {
        std::atomic<std::uint64_t> fingerprint;
        std::atomic<std::uint64_t> offset;
    };
    static_assert(sizeof(Entry) == 16, "Entry must be 16 bytes");

    struct alignas(128) Bucket {
        // Bit layout in `header`:
        //   [0..6]   occupancy bitmap (1 = entry slot used)
        //   [7]      has_overflow (chain non-empty)
        //   [8..63]  next_overflow_idx (1-based; 0 means none)
        std::atomic<std::uint64_t> header;
        Entry entries[kEntriesPerBucket];
        std::uint64_t pad;
    };
    static_assert(sizeof(Bucket) == kBucketSize, "Bucket must be 128 bytes");

    struct RegionHeader {
        std::atomic<std::uint64_t> num_entries;        // best-effort live counter
        std::atomic<std::uint64_t> next_overflow_alloc; // 1-based; 0 reserved as "no chain"
    };
    static_assert(sizeof(RegionHeader) == 16, "RegionHeader must be 16 bytes");

    struct GlobalHeader {
        std::uint64_t magic;
        std::uint32_t version;
        std::uint32_t pad0;
        std::uint64_t num_regions;
        std::uint64_t main_buckets_per_region;
        std::uint64_t overflow_slots_per_region;
        std::uint64_t total_size;
        std::uint64_t reserved[10];
    };
    static_assert(sizeof(GlobalHeader) <= 4096,
                  "GlobalHeader must fit in first PAGE");

    static std::unique_ptr<ColdTier> create(
            const std::string& pool_file,
            std::size_t main_buckets_per_region = kDefaultMainBuckets,
            std::size_t overflow_slots_per_region = kDefaultOverflowSlots) {
        // Reserve overflow_idx=0 as "no chain" sentinel — actual indices
        // start at 1. So we reserve one extra Bucket slot at the bottom
        // of the overflow pool that's never allocated.
        const std::size_t region_buckets
            = main_buckets_per_region + overflow_slots_per_region + 1;
        const std::size_t region_size
            = sizeof(RegionHeader) + region_buckets * sizeof(Bucket);
        const std::size_t total_size = 4096 + kNumRegions * region_size;

        const int fd = ::open(pool_file.c_str(),
                              O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) throw std::runtime_error(
            "ColdTier::create: open failed: " + pool_file);
        if (::ftruncate(fd, total_size) != 0) {
            ::close(fd);
            throw std::runtime_error("ColdTier::create: ftruncate failed");
        }
        void* base = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED) throw std::runtime_error(
            "ColdTier::create: mmap failed");
        std::memset(base, 0, total_size);

        auto* hdr = static_cast<GlobalHeader*>(base);
        hdr->magic = kMagic;
        hdr->version = kVersion;
        hdr->pad0 = 0;
        hdr->num_regions = kNumRegions;
        hdr->main_buckets_per_region = main_buckets_per_region;
        hdr->overflow_slots_per_region = overflow_slots_per_region;
        hdr->total_size = total_size;
        viper::internal::pmem_persist(hdr, sizeof(*hdr));

        return std::unique_ptr<ColdTier>(
            new ColdTier(pool_file, base, total_size,
                         main_buckets_per_region, overflow_slots_per_region));
    }

    static std::unique_ptr<ColdTier> open(const std::string& pool_file) {
        const int fd = ::open(pool_file.c_str(), O_RDWR, 0644);
        if (fd < 0) throw std::runtime_error(
            "ColdTier::open: open failed: " + pool_file);
        struct stat st;
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("ColdTier::open: fstat failed");
        }
        const std::size_t total_size = st.st_size;
        void* base = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED) throw std::runtime_error(
            "ColdTier::open: mmap failed");

        auto* hdr = static_cast<GlobalHeader*>(base);
        if (hdr->magic != kMagic)
            throw std::runtime_error("ColdTier::open: bad magic");
        if (hdr->version != kVersion)
            throw std::runtime_error("ColdTier::open: version mismatch");
        if (hdr->num_regions != kNumRegions)
            throw std::runtime_error("ColdTier::open: num_regions mismatch");
        if (hdr->total_size != total_size)
            throw std::runtime_error("ColdTier::open: size mismatch");

        return std::unique_ptr<ColdTier>(
            new ColdTier(pool_file, base, total_size,
                         hdr->main_buckets_per_region,
                         hdr->overflow_slots_per_region));
    }

    ~ColdTier() {
        if (base_) ::munmap(base_, total_size_);
    }
    ColdTier(const ColdTier&) = delete;
    ColdTier& operator=(const ColdTier&) = delete;

    bool upsert(std::uint64_t fingerprint, Offset off) {
        assert(fingerprint != kEmptyFp);
        assert(off.offset != kTombstoneOffset);
        const std::size_t rid = region_id_of(fingerprint);
        Bucket& head = main_bucket_of(rid, fingerprint);

        // Single-pass walk of the chain: look for a matching fp (update
        // path), and along the way remember the first empty slot we
        // saw. If the chain ends without a match, we claim that slot;
        // if the entire chain is full of distinct fps, we extend.
        //
        // Note: Phase B-1 stress tests use disjoint keys per thread, so
        // duplicate-fp races during concurrent insert can't occur. If
        // such workloads matter later, Phase B-2 will add a per-chain
        // lock between "no match found" and "claim/attach".
        Bucket* found_empty_bucket = nullptr;
        std::size_t found_empty_idx = 0;
        Bucket* cur = &head;
        Bucket* tail = nullptr;
        while (cur != nullptr) {
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                const std::uint64_t fp = cur->entries[i].fingerprint.load(
                    std::memory_order_acquire);
                if (fp == fingerprint) {
                    cur->entries[i].offset.store(off.offset,
                                                 std::memory_order_release);
                    viper::internal::pmem_persist(&cur->entries[i].offset,
                                                  sizeof(std::uint64_t));
                    return true;
                }
                if (fp == kEmptyFp && found_empty_bucket == nullptr) {
                    found_empty_bucket = cur;
                    found_empty_idx = i;
                }
            }
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) { tail = cur; break; }
            cur = overflow_bucket_of(rid, next_idx);
        }

        // No match anywhere. Try to claim the first empty slot we found.
        if (found_empty_bucket != nullptr) {
            std::uint64_t expected = kEmptyFp;
            if (found_empty_bucket->entries[found_empty_idx].fingerprint
                    .compare_exchange_strong(
                        expected, fingerprint,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                found_empty_bucket->entries[found_empty_idx].offset.store(
                    off.offset, std::memory_order_release);
                viper::internal::pmem_persist(
                    &found_empty_bucket->entries[found_empty_idx], sizeof(Entry));
                const std::uint64_t bit = std::uint64_t{1} << found_empty_idx;
                found_empty_bucket->header.fetch_or(bit,
                                                    std::memory_order_acq_rel);
                viper::internal::pmem_persist(
                    &found_empty_bucket->header,
                    sizeof(found_empty_bucket->header));
                region_at(rid).num_entries.fetch_add(
                    1, std::memory_order_relaxed);
                return true;
            }
            // Lost the slot to a concurrent inserter; retry the whole
            // upsert (rare, no infinite-loop concern).
            return upsert(fingerprint, off);
        }

        // Phase 3: extend chain. Allocate a fresh overflow bucket.
        const std::uint64_t new_idx
            = region_at(rid).next_overflow_alloc.fetch_add(
                  1, std::memory_order_acq_rel) + 1;  // 1-based
        if (new_idx > overflow_slots_per_region_) {
            // Pool exhausted (Phase B-2 split would prevent this).
            region_at(rid).next_overflow_alloc.fetch_sub(
                1, std::memory_order_acq_rel);
            return false;
        }
        Bucket* nb = overflow_bucket_of(rid, new_idx);
        // The new bucket starts zeroed (the file was memset on create);
        // claim slot 0 for our entry.
        nb->entries[0].fingerprint.store(fingerprint,
                                         std::memory_order_release);
        nb->entries[0].offset.store(off.offset,
                                    std::memory_order_release);
        viper::internal::pmem_persist(&nb->entries[0], sizeof(Entry));
        nb->header.store(0x1ull,  // bit 0 set in occupancy, no overflow yet
                         std::memory_order_release);
        viper::internal::pmem_persist(&nb->header, sizeof(nb->header));
        viper::internal::pmem_persist(&nb->pad, sizeof(nb->pad));  // dummy

        // Attach the new bucket to the chain tail. Use CAS to handle
        // concurrent extenders: if we lose, rewalk from `tail` to find
        // the new tail and try again.
        const std::uint64_t attach_bits = (1ull << 7) | (new_idx << 8);
        while (true) {
            const std::uint64_t cur_hdr
                = tail->header.load(std::memory_order_acquire);
            if ((cur_hdr >> 8) != kNoOverflow) {
                // Someone else already extended. Walk to new tail and
                // retry chain-walk + claim there.
                Bucket* deeper = overflow_bucket_of(rid, cur_hdr >> 8);
                // Try to claim a slot in their bucket; if their bucket
                // is also full, we'll recurse via tail-walking. We
                // already wrote OUR own bucket and persisted it; we
                // don't want to lose it. So instead, attach our bucket
                // BELOW theirs by recursing down their chain.
                tail = deeper;
                continue;
            }
            const std::uint64_t desired = (cur_hdr & 0x7full) | attach_bits;
            std::uint64_t expected = cur_hdr;
            if (tail->header.compare_exchange_strong(
                    expected, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                viper::internal::pmem_persist(&tail->header,
                                              sizeof(tail->header));
                region_at(rid).num_entries.fetch_add(
                    1, std::memory_order_relaxed);
                return true;
            }
            // CAS failed — someone modified `tail->header`. Loop and
            // re-read; if next_overflow_idx now non-zero, walk deeper.
        }
    }

    // -- M3 follow-up: bulk_upsert with amortised PM fence cost --------
    //
    // Same semantics as repeated calls to `upsert`, but groups entries
    // hitting the same bucket and emits ONE sfence per bucket-group
    // (instead of two per entry). Contract: caller must hold an
    // external mutex (e.g., HiOM::apply_mu_) so no concurrent writer
    // hits the same chain. Concurrent readers and concurrent legacy
    // `upsert` from threads that are NOT writers (e.g., the M6 tail-
    // scan workers, which are gated by HiOM constructor sequencing) are
    // safe.
    //
    // Internally:
    //   1. Sort `entries` by (region, bucket).
    //   2. For each contiguous (region, bucket) group:
    //      a. Walk the chain once, classifying each entry as
    //         match-update (existing fp) or new-insert (claim empty
    //         slot or extend chain).
    //      b. Stage all writes in DRAM (atomic stores to PM-backed
    //         memory take effect immediately, but only become durable
    //         after clwb+sfence; we omit per-entry sfences).
    //      c. clwb every dirtied cache line.
    //      d. ONE sfence to commit the entry-data half.
    //      e. OR all new occupancy bits into the header.
    //      f. clwb the header lines + ONE sfence to commit the
    //         occupancy half.
    //   3. The two-fence-per-bucket pattern preserves the
    //      crash-consistency invariant: header bit visible only after
    //      entry data is durable. (Same invariant as `upsert`.)
    //
    // Returns the number of entries written (== entries.size() unless
    // an overflow pool ran out; partial failures abort the rest of the
    // group).
    struct BulkEntry {
        std::uint64_t fp;
        Offset off;
    };

    std::size_t bulk_upsert(std::vector<BulkEntry>& entries) {
        if (entries.empty()) return 0;
        // Sort by (region, bucket) so same-bucket entries cluster.
        // Stable not required; no equal-key duplicates are passed in
        // (HiOM::apply_batch coalesces by fp64 before calling).
        std::sort(entries.begin(), entries.end(),
                  [this](const BulkEntry& a, const BulkEntry& b) {
                      const auto ra = region_id_of(a.fp);
                      const auto rb = region_id_of(b.fp);
                      if (ra != rb) return ra < rb;
                      return bucket_id_of(a.fp) < bucket_id_of(b.fp);
                  });

        std::size_t total_written = 0;
        std::size_t i = 0;
        while (i < entries.size()) {
            const std::size_t rid = region_id_of(entries[i].fp);
            const std::size_t bid = bucket_id_of(entries[i].fp);
            // Find the run of entries hitting this (rid, bid).
            std::size_t j = i + 1;
            while (j < entries.size()
                   && region_id_of(entries[j].fp) == rid
                   && bucket_id_of(entries[j].fp) == bid) {
                ++j;
            }
            total_written += apply_bucket_group(rid, bid, &entries[i], j - i);
            i = j;
        }
        return total_written;
    }
    // ------------------------------------------------------------------

    std::optional<Offset> lookup(std::uint64_t fingerprint) const {
        assert(fingerprint != kEmptyFp);
        const std::size_t rid = region_id_of(fingerprint);
        const Bucket* cur = &main_bucket_of(rid, fingerprint);
        while (cur != nullptr) {
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                const std::uint64_t fp = cur->entries[i].fingerprint.load(
                    std::memory_order_acquire);
                if (fp == fingerprint) {
                    const std::uint64_t off
                        = cur->entries[i].offset.load(
                              std::memory_order_acquire);
                    if (off == kTombstoneOffset) return std::nullopt;
                    return Offset{off};
                }
            }
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) break;
            cur = overflow_bucket_of(rid, next_idx);
        }
        return std::nullopt;
    }

    bool remove(std::uint64_t fingerprint) {
        assert(fingerprint != kEmptyFp);
        const std::size_t rid = region_id_of(fingerprint);
        Bucket* cur = &main_bucket_of(rid, fingerprint);
        while (cur != nullptr) {
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                const std::uint64_t fp = cur->entries[i].fingerprint.load(
                    std::memory_order_acquire);
                if (fp == fingerprint) {
                    cur->entries[i].offset.store(
                        kTombstoneOffset, std::memory_order_release);
                    viper::internal::pmem_persist(
                        &cur->entries[i].offset, sizeof(std::uint64_t));
                    return true;
                }
            }
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) break;
            cur = overflow_bucket_of(rid, next_idx);
        }
        return false;
    }

    // Region-parallel scan. Each thread takes a strided subset of the
    // 32 regions and applies `visitor(fingerprint, offset)` to every
    // live (non-tombstone) entry. Used as the entry point for HiOM
    // recovery (e.g., warming the hot tier).
    template <typename Visitor>
    void parallel_load(std::size_t num_threads, Visitor&& visitor) const {
        if (num_threads == 0) num_threads = 1;
        if (num_threads > kNumRegions) num_threads = kNumRegions;
        std::vector<std::thread> workers;
        workers.reserve(num_threads);
        for (std::size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([this, t, num_threads, &visitor]() {
                for (std::size_t r = t; r < kNumRegions; r += num_threads) {
                    scan_region(r, visitor);
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Single-region scan helper exposed for tests / benchmarks.
    template <typename Visitor>
    void scan_region(std::size_t rid, Visitor& visitor) const {
        for (std::size_t b = 0; b < main_buckets_per_region_; ++b) {
            scan_chain(&main_buckets_of(rid)[b], rid, visitor);
        }
    }

    std::size_t num_regions() const { return kNumRegions; }
    std::size_t main_buckets_per_region() const {
        return main_buckets_per_region_;
    }
    std::size_t overflow_slots_per_region() const {
        return overflow_slots_per_region_;
    }
    std::size_t capacity_main_entries() const {
        return kNumRegions * main_buckets_per_region_ * kEntriesPerBucket;
    }
    std::size_t approx_size() const {
        std::size_t total = 0;
        for (std::size_t r = 0; r < kNumRegions; ++r) {
            total += region_at(r).num_entries.load(std::memory_order_relaxed);
        }
        return total;
    }
    std::size_t approx_overflow_used() const {
        std::size_t total = 0;
        for (std::size_t r = 0; r < kNumRegions; ++r) {
            total += region_at(r).next_overflow_alloc.load(
                std::memory_order_relaxed);
        }
        return total;
    }

    // White-box DRAM footprint. The index (regions/buckets/overflow pool)
    // lives entirely in the PMem mmap at base_ and is NOT counted as DRAM;
    // only the control struct + pool path string are DRAM-resident, so this
    // is ≈0 by design (the point of tiering the cold index onto PMem).
    std::size_t dram_bytes() const {
        return sizeof(*this) + pool_file_.capacity();
    }

  private:
    ColdTier(const std::string& pool_file, void* base,
             std::size_t total_size,
             std::size_t main_buckets_per_region,
             std::size_t overflow_slots_per_region)
        : pool_file_(pool_file),
          base_(base),
          total_size_(total_size),
          main_buckets_per_region_(main_buckets_per_region),
          overflow_slots_per_region_(overflow_slots_per_region),
          region_buckets_total_(main_buckets_per_region
                                + overflow_slots_per_region + 1),
          region_size_(sizeof(RegionHeader)
                       + (main_buckets_per_region
                          + overflow_slots_per_region + 1) * sizeof(Bucket)),
          regions_base_(static_cast<char*>(base) + 4096) {}

    static std::size_t region_id_of(std::uint64_t fp) {
        return static_cast<std::size_t>(fp >> 59) & (kNumRegions - 1);
    }
    std::size_t bucket_id_of(std::uint64_t fp) const {
        std::uint64_t h = fp;
        h ^= h >> 17;
        h *= 0x9e3779b97f4a7c15ull;
        h ^= h >> 27;
        return static_cast<std::size_t>(h) & (main_buckets_per_region_ - 1);
    }

    char* region_ptr(std::size_t rid) const {
        return regions_base_ + rid * region_size_;
    }
    RegionHeader& region_at(std::size_t rid) const {
        return *reinterpret_cast<RegionHeader*>(region_ptr(rid));
    }
    Bucket* main_buckets_of(std::size_t rid) const {
        return reinterpret_cast<Bucket*>(region_ptr(rid)
                                         + sizeof(RegionHeader));
    }
    Bucket& main_bucket_of(std::size_t rid, std::uint64_t fp) const {
        return main_buckets_of(rid)[bucket_id_of(fp)];
    }
    // Overflow buckets sit immediately after main buckets; index is 1-based,
    // so idx==1 means the first overflow Bucket. idx==0 is the reserved
    // sentinel slot (= "no overflow").
    Bucket* overflow_bucket_of(std::size_t rid, std::uint64_t idx) const {
        return main_buckets_of(rid) + main_buckets_per_region_ + idx;
    }

    bool chain_update_match(Bucket& head, std::uint64_t fingerprint,
                            Offset off) {
        Bucket* cur = &head;
        while (cur != nullptr) {
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                const std::uint64_t fp = cur->entries[i].fingerprint.load(
                    std::memory_order_acquire);
                if (fp == fingerprint) {
                    cur->entries[i].offset.store(
                        off.offset, std::memory_order_release);
                    viper::internal::pmem_persist(
                        &cur->entries[i].offset, sizeof(std::uint64_t));
                    return true;
                }
            }
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) break;
            cur = overflow_bucket_of(region_id_of(fingerprint), next_idx);
        }
        return false;
    }

    bool try_claim_empty(Bucket& b, std::uint64_t fingerprint, Offset off) {
        for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
            const std::uint64_t fp = b.entries[i].fingerprint.load(
                std::memory_order_acquire);
            if (fp != kEmptyFp) continue;
            std::uint64_t expected = kEmptyFp;
            if (!b.entries[i].fingerprint.compare_exchange_strong(
                    expected, fingerprint,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) continue;
            // We own the slot; finish writing.
            b.entries[i].offset.store(off.offset, std::memory_order_release);
            viper::internal::pmem_persist(&b.entries[i], sizeof(Entry));
            const std::uint64_t bit = std::uint64_t{1} << i;
            b.header.fetch_or(bit, std::memory_order_acq_rel);
            viper::internal::pmem_persist(&b.header, sizeof(b.header));
            return true;
        }
        return false;
    }

    // Apply a contiguous run of `n` BulkEntry that all hash to
    // (rid, bid). Caller (bulk_upsert) holds a logical writer lock —
    // for our usage (HiOM::apply_batch under apply_mu_) no concurrent
    // bulk_upsert / upsert from another writer hits the same chain.
    // Returns the number of entries successfully written; partial
    // failure on overflow-pool exhaustion stops the rest of the group.
    //
    // Two-phase persistence per bucket touched, mirroring upsert's
    // crash-consistency contract (entry data durable BEFORE occupancy
    // bit becomes visible):
    //
    //   Phase 1: stage all entry stores (fp + offset) to dirty cache
    //            lines, accumulate `pending_bits` per Bucket*. Flush
    //            every dirtied entry line with pmem_flush_range, then
    //            ONE pmem_drain commits the whole entry-data half.
    //   Phase 2: header.fetch_or(pending_bits) for each dirtied
    //            bucket, flush the header line, then ONE pmem_drain
    //            commits the occupancy half.
    //
    // Per-fp updates (existing fingerprint match) take a faster
    // single-phase path — just one entry-line flush + the shared
    // drain. They never need a header bit flip (occupancy is already 1).
    std::size_t apply_bucket_group(std::size_t rid, std::size_t bid,
                                   const BulkEntry* entries, std::size_t n) {
        if (n == 0) return 0;
        Bucket& head = main_buckets_of(rid)[bid];

        // We collect "dirty buckets" in the order we touch them so we
        // can flush them once at the end. A small inline-vector would
        // be ideal; for typical group sizes (1..few) the std::vector
        // allocation is amortised by the bulk_upsert savings.
        struct DirtyBucket {
            Bucket* b;
            std::uint64_t add_bits;  // bits to OR into header.occupancy
        };
        std::vector<DirtyBucket> dirty;
        dirty.reserve(4);

        auto find_or_create_dirty = [&](Bucket* b) -> DirtyBucket& {
            for (auto& d : dirty) if (d.b == b) return d;
            dirty.push_back({b, 0ull});
            return dirty.back();
        };

        std::size_t written = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint64_t fp = entries[k].fp;
            const Offset off = entries[k].off;
            assert(fp != kEmptyFp);
            assert(off.offset != kTombstoneOffset);

            // Walk the chain: look for fp match, remember first empty
            // slot we see along the way.
            Bucket* cur = &head;
            Bucket* tail = &head;
            Bucket* found_empty_b = nullptr;
            std::size_t found_empty_i = 0;
            bool matched = false;
            while (cur != nullptr) {
                for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                    const std::uint64_t cfp
                        = cur->entries[i].fingerprint.load(
                              std::memory_order_acquire);
                    if (cfp == fp) {
                        // Update path: just rewrite the offset slot.
                        cur->entries[i].offset.store(
                            off.offset, std::memory_order_release);
                        viper::internal::pmem_flush_range(
                            &cur->entries[i].offset,
                            sizeof(std::uint64_t));
                        // Track the dirty bucket so the final drain
                        // covers this line; no header bit needed.
                        find_or_create_dirty(cur);
                        matched = true;
                        ++written;
                        break;
                    }
                    if (cfp == kEmptyFp && found_empty_b == nullptr) {
                        found_empty_b = cur;
                        found_empty_i = i;
                    }
                }
                if (matched) break;
                tail = cur;
                const std::uint64_t hdr
                    = cur->header.load(std::memory_order_acquire);
                const std::uint64_t next_idx = hdr >> 8;
                if (next_idx == kNoOverflow) break;
                cur = overflow_bucket_of(rid, next_idx);
            }
            if (matched) continue;

            // Insert path. Try first remembered empty slot.
            if (found_empty_b != nullptr) {
                std::uint64_t expected = kEmptyFp;
                if (found_empty_b->entries[found_empty_i].fingerprint
                        .compare_exchange_strong(
                            expected, fp,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                    found_empty_b->entries[found_empty_i].offset.store(
                        off.offset, std::memory_order_release);
                    viper::internal::pmem_flush_range(
                        &found_empty_b->entries[found_empty_i],
                        sizeof(Entry));
                    auto& d = find_or_create_dirty(found_empty_b);
                    d.add_bits |= (std::uint64_t{1} << found_empty_i);
                    region_at(rid).num_entries.fetch_add(
                        1, std::memory_order_relaxed);
                    ++written;
                    continue;
                }
                // Lost the slot (concurrent inserter). Fall through to
                // overflow allocation — preserves "every entry lands"
                // contract and matches single-key upsert's recursion.
            }

            // Need to extend the chain with a fresh overflow bucket.
            const std::uint64_t alloc_idx
                = region_at(rid).next_overflow_alloc.fetch_add(
                      1, std::memory_order_acq_rel) + 1;
            if (alloc_idx > overflow_slots_per_region_) {
                region_at(rid).next_overflow_alloc.fetch_sub(
                    1, std::memory_order_acq_rel);
                // Pool exhausted — abort the rest of the group.
                break;
            }
            Bucket* nb = overflow_bucket_of(rid, alloc_idx);
            nb->entries[0].fingerprint.store(fp, std::memory_order_release);
            nb->entries[0].offset.store(off.offset,
                                        std::memory_order_release);
            viper::internal::pmem_flush_range(&nb->entries[0],
                                              sizeof(Entry));
            // The new bucket is its own "dirty bucket"; its add_bits
            // includes bit-0 (the entry we just placed). The chain
            // attach to `tail` is recorded as a separate dirty
            // touching tail->header.
            auto& dnb = find_or_create_dirty(nb);
            dnb.add_bits |= 0x1ull;
            // Attach: tail->header gets has_overflow + next_idx. We
            // can't do this with fetch_or because the next-idx bits
            // need to land atomically with the has_overflow flag; do
            // a CAS loop that's normally one-shot under apply_mu_.
            const std::uint64_t attach_bits = (1ull << 7) | (alloc_idx << 8);
            while (true) {
                const std::uint64_t cur_hdr
                    = tail->header.load(std::memory_order_acquire);
                if ((cur_hdr >> 8) != kNoOverflow) {
                    // Some other writer (legacy single-key upsert from a
                    // recovery worker, say) already attached. Walk down
                    // to the new tail and retry the attach there.
                    tail = overflow_bucket_of(rid, cur_hdr >> 8);
                    continue;
                }
                const std::uint64_t desired
                    = (cur_hdr & 0x7full) | attach_bits;
                std::uint64_t expected = cur_hdr;
                if (tail->header.compare_exchange_strong(
                        expected, desired,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // Don't pmem_persist tail->header here — defer to
                    // Phase 2's batched flush.
                    auto& dt = find_or_create_dirty(tail);
                    // No new occupancy bits on tail; the attach bits
                    // are already applied via the CAS itself.
                    (void)dt;
                    break;
                }
                // Lost the CAS, retry.
            }
            region_at(rid).num_entries.fetch_add(
                1, std::memory_order_relaxed);
            ++written;
        }

        if (dirty.empty()) return written;

        // Phase 1 drain: commit all entry-data flushes issued above.
        viper::internal::pmem_drain();

        // Phase 2: flip occupancy bits and persist headers. Headers
        // live on a separate cache line from entries (Bucket layout:
        // header at offset 0; entries start at 8). One flush_range
        // covering the header word is enough; we batch the drain.
        for (auto& d : dirty) {
            if (d.add_bits != 0ull) {
                d.b->header.fetch_or(d.add_bits,
                                     std::memory_order_acq_rel);
            }
            // Flush the cache line holding `header` regardless — chain
            // attach (CAS above) may have also modified it without
            // any add_bits update.
            viper::internal::pmem_flush_range(&d.b->header,
                                              sizeof(d.b->header));
        }
        viper::internal::pmem_drain();
        return written;
    }

    template <typename Visitor>
    void scan_chain(const Bucket* head, std::size_t rid, Visitor& visitor) const {
        const Bucket* cur = head;
        while (cur != nullptr) {
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                if (((hdr >> i) & 1ull) == 0) continue;
                const std::uint64_t fp
                    = cur->entries[i].fingerprint.load(std::memory_order_acquire);
                if (fp == kEmptyFp) continue;
                const std::uint64_t off
                    = cur->entries[i].offset.load(std::memory_order_acquire);
                if (off == kTombstoneOffset) continue;
                visitor(fp, Offset{off});
            }
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) break;
            cur = overflow_bucket_of(rid, next_idx);
        }
    }

    std::string pool_file_;
    void* base_;
    std::size_t total_size_;
    std::size_t main_buckets_per_region_;
    std::size_t overflow_slots_per_region_;
    std::size_t region_buckets_total_;  // main + overflow + 1 sentinel
    std::size_t region_size_;
    char* regions_base_;
};

}  // namespace viper::hiom
