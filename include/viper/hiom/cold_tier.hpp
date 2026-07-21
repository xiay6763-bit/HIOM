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
//   - Entry: 16 B = 8 B `key_id` + 8 B `offset` (full
//     viper::KeyValueOffset). `key_id` is the full key (raw bytes, ≤8 B);
//     region/bucket routing uses a separate fp64 the caller passes in, so
//     distinct keys that share a routing hash coexist as separate entries
//     rather than aliasing. Match/dedup is on the exact key_id.
//   - Occupancy bit is the SOLE visibility authority. A slot is
//     published (visible to lookup/remove/scan, update-in-place) iff its
//     header occupancy bit is 1. A slot with bit 0 is invisible and
//     reusable, WHETHER OR NOT its key_id is zero — a bit=0 slot is crash
//     residue (entry durable, header flip not reached) and is safely
//     completed (same key_id re-inserted → republish) or reclaimed
//     (different key_id → overwrite) by the next insert into that chain.
//   - Update: 8 B store on the offset field of a PUBLISHED slot — Optane
//     ADR provides 8 B atomic durability; clwb+sfence for ordering.
//   - Insert: write entry first, persist; THEN flip occupancy bit and
//     persist header. Crash with bit unset → slot invisible & reusable.
//     Same pattern as Viper's VPage write-then-bitset
//     (viper.hpp:1037-1054).
//   - Delete: 8 B store the offset of a published slot to
//     kTombstoneOffset, persist. The key_id stays in place so a
//     re-insert finds the slot via match-and-update.
//   - Concurrency: one publisher per region via region_mus_[rid], taken
//     by every writer (upsert / remove / bulk_upsert). Readers are
//     lock-free and rely only on the occupancy-bit-set-last ordering.
//     This is the single-publisher invariant that makes reusing a bit=0
//     residue slot safe. (Recovery replay is region-overlapping but each
//     upsert takes the region lock; steady-state flushers own disjoint
//     regions so the lock is uncontended.)
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
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/mman.h>
#include <mutex>
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
    static constexpr std::uint32_t kVersion = 3;                    // bumped: entries store full key_id (not fp64)
    static constexpr std::size_t kNumRegions = 32;
    static constexpr std::size_t kEntriesPerBucket = 7;
    static constexpr std::size_t kBucketSize = 128;
    static constexpr std::uint64_t kEmptyFp = 0;
    static constexpr std::uint64_t kTombstoneOffset = 0xFFFF'FFFF'FFFF'FFFFull;
    static constexpr std::uint64_t kNoOverflow = 0;  // 0 means "none" (idx 0 reserved)

    // Small safety-net defaults — callers that know their key count MUST
    // size via sizing_for() (fixtures, recovery_bm, probes all do). These
    // exist only so a forgotten sizing surfaces as a fail-fast overflow
    // abort rather than a huge pre-allocated file: kept intentionally
    // small so no path silently over-allocates ~1 GiB of PM.
    static constexpr std::size_t kDefaultMainBuckets = 8192;
    static constexpr std::size_t kDefaultOverflowSlots = 16384;  // 2x main

#ifdef VIPER_COLD_FAULT_INJECT
    // Test-only crash-point injection (compiled ONLY into cold_tier_test
    // via -DVIPER_COLD_FAULT_INJECT — the steady write path carries no
    // permanent branch). Arm a point, issue one upsert; the write persists
    // up to that point then returns early, leaving PM in the partial state
    // a real crash would. The test reopens and asserts visibility.
    enum class FaultPoint {
        kNone,
        kAfterKeyIdBeforeEntry,     // key_id persisted; offset + occupancy not
        kAfterEntryBeforeBit,       // entry persisted; occupancy bit not set
        kAfterOverflowBeforeChain,  // overflow bucket persisted; not linked
    };
    static inline thread_local FaultPoint fault_point_ = FaultPoint::kNone;
    static void arm_fault(FaultPoint p) { fault_point_ = p; }
    static void disarm_fault() { fault_point_ = FaultPoint::kNone; }
#endif

    struct Entry {
        // Full-key identity: the raw key bytes (fixed-size keys ≤ 8 B,
        // zero-padded into a machine word). Replaces the old 64-bit
        // fingerprint. Entries are matched on this EXACT value, so two
        // distinct keys that happen to share a routing hash never alias —
        // correctness no longer depends on a hash-collision probability.
        // Routing (region/bucket) uses a separate fp64 the caller passes in
        // (see upsert/lookup/remove), not this field.
        std::atomic<std::uint64_t> key_id;
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

    // Round up to the next power of two (>= 1). Local helper — the repo
    // builds at C++17, so std::bit_ceil (C++20) isn't available.
    static std::size_t next_pow2(std::size_t v) {
        if (v <= 1) return 1;
        return std::size_t{1} << (64 - __builtin_clzll(v - 1));
    }

    // Compute (main_buckets, overflow_slots) per region to hold roughly
    // `expected_keys` total live entries at a target load factor, with
    // headroom. main_buckets is the addressable table (must be pow2 —
    // create() rounds anyway); overflow absorbs chain spill. Every
    // caller that knows its key count should size through this so PM
    // footprint tracks the workload instead of the safety-net default.
    struct Sizing {
        std::size_t main_buckets;
        std::size_t overflow_slots;
    };
    static Sizing sizing_for(std::size_t expected_keys) {
        // Slots needed per region at target load; +1 to round up.
        constexpr double kTargetLoad = 0.5;
        const double slots_per_region
            = static_cast<double>(expected_keys)
              / static_cast<double>(kNumRegions);
        std::size_t main = static_cast<std::size_t>(
            slots_per_region / (kEntriesPerBucket * kTargetLoad)) + 1;
        main = next_pow2(std::max<std::size_t>(8192, main));
        // Overflow pool sized to match main — generous, since chains
        // only form under fingerprint clustering, not raw load.
        return Sizing{main, main};
    }

    static std::unique_ptr<ColdTier> create(
            const std::string& pool_file,
            std::size_t main_buckets_per_region = kDefaultMainBuckets,
            std::size_t overflow_slots_per_region = kDefaultOverflowSlots) {
        // bucket_id_of() masks with (main_buckets - 1), so main_buckets
        // MUST be a power of two or a large fraction of the table is
        // unaddressable (silent load-factor blowup). Round both pools up
        // to the next power of two here — one guard covers every caller
        // (fixtures, recovery_bm, probes) regardless of the sizing
        // arithmetic they used.
        // Only main_buckets is masked (bucket_id_of uses & (main-1)), so
        // ONLY it must be a power of two. The overflow pool is a linear
        // bump allocator (next_overflow_alloc fetch_add) — rounding it up
        // would just waste PM, so leave it exactly as requested.
        main_buckets_per_region
            = next_pow2(std::max<std::size_t>(1, main_buckets_per_region));
        overflow_slots_per_region
            = std::max<std::size_t>(1, overflow_slots_per_region);
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

    bool upsert(std::uint64_t fp64_route, std::uint64_t key_id, Offset off) {
        assert(off.offset != kTombstoneOffset);
        const std::size_t rid = region_id_of(fp64_route);
        // Writer isolation: one publisher per region. Recovery replay is
        // block-sharded (region-overlapping) and direct-upsert stress
        // tests collide routes across regions, so without this lock two
        // threads could race to publish the same slot. Steady-state
        // flushers own disjoint regions, so this lock is uncontended for
        // them. Readers never take it.
        std::lock_guard<std::mutex> lk(region_mus_[rid]);
        Bucket& head = main_bucket_of(rid, fp64_route);

        // Single-pass walk of the chain. Occupancy is authoritative:
        //  - published (bit=1) + key_id match -> update offset in place.
        //  - not published (bit=0)            -> reusable (empty OR crash
        //    residue); remember the first one. We only claim it AFTER
        //    confirming no published match exists anywhere in the chain
        //    (else we'd create a duplicate key).
        Bucket* found_reusable_bucket = nullptr;
        std::size_t found_reusable_idx = 0;
        Bucket* cur = &head;
        Bucket* tail = nullptr;
        while (cur != nullptr) {
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                if (slot_published(hdr, i)) {
                    const std::uint64_t kid = cur->entries[i].key_id.load(
                        std::memory_order_acquire);
                    if (kid == key_id) {
                        cur->entries[i].offset.store(off.offset,
                                                     std::memory_order_release);
                        viper::internal::pmem_persist(&cur->entries[i].offset,
                                                      sizeof(std::uint64_t));
                        return true;
                    }
                } else if (found_reusable_bucket == nullptr) {
                    found_reusable_bucket = cur;
                    found_reusable_idx = i;
                }
            }
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) { tail = cur; break; }
            cur = overflow_bucket_of(rid, next_idx);
        }

        // No published match. Claim the first reusable slot. Under the
        // region lock there is no concurrent publisher, so plain stores
        // suffice (no CAS): write key_id+offset, persist the entry, THEN set
        // the occupancy bit and persist the header. A crash between the
        // two persists leaves bit=0 -> invisible & reusable next time. A
        // lock-free reader gates on the bit (set last, release), so it
        // never observes the transient overwrite of a stale residue key_id.
        if (found_reusable_bucket != nullptr) {
            Bucket* b = found_reusable_bucket;
            const std::size_t idx = found_reusable_idx;
            b->entries[idx].key_id.store(key_id,
                                         std::memory_order_release);
#ifdef VIPER_COLD_FAULT_INJECT
            if (fault_point_ == FaultPoint::kAfterKeyIdBeforeEntry) {
                // key_id durable, offset + occupancy bit not.
                viper::internal::pmem_persist(&b->entries[idx].key_id,
                                              sizeof(std::uint64_t));
                return false;
            }
#endif
            b->entries[idx].offset.store(off.offset, std::memory_order_release);
            viper::internal::pmem_persist(&b->entries[idx], sizeof(Entry));
#ifdef VIPER_COLD_FAULT_INJECT
            if (fault_point_ == FaultPoint::kAfterEntryBeforeBit) {
                // entry durable, occupancy bit not set.
                return false;
            }
#endif
            const std::uint64_t bit = std::uint64_t{1} << idx;
            b->header.fetch_or(bit, std::memory_order_acq_rel);
            viper::internal::pmem_persist(&b->header, sizeof(b->header));
            region_at(rid).num_entries.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Phase 3: extend chain with a fresh overflow bucket. Region lock
        // held, so no concurrent extender — the CAS attach dance is not
        // needed; `tail` is stable.
        const std::uint64_t new_idx
            = region_at(rid).next_overflow_alloc.fetch_add(
                  1, std::memory_order_acq_rel) + 1;  // 1-based
        if (new_idx > overflow_slots_per_region_) {
            // Pool exhausted (online split would prevent this).
            region_at(rid).next_overflow_alloc.fetch_sub(
                1, std::memory_order_acq_rel);
            return false;
        }
        Bucket* nb = overflow_bucket_of(rid, new_idx);
        // The new bucket starts zeroed (the file was memset on create);
        // claim slot 0 for our entry, publish (bit 0), persist. The
        // chain attach comes LAST so a crash before it leaves the new
        // bucket unreachable = invisible (correct).
        nb->entries[0].key_id.store(key_id,
                                    std::memory_order_release);
        nb->entries[0].offset.store(off.offset,
                                    std::memory_order_release);
        viper::internal::pmem_persist(&nb->entries[0], sizeof(Entry));
        nb->header.store(0x1ull,  // bit 0 set in occupancy, no overflow yet
                         std::memory_order_release);
        viper::internal::pmem_persist(&nb->header, sizeof(nb->header));
#ifdef VIPER_COLD_FAULT_INJECT
        if (fault_point_ == FaultPoint::kAfterOverflowBeforeChain) {
            // overflow bucket fully written + published, but the tail's
            // next_overflow link is not stored -> bucket unreachable.
            return false;
        }
#endif

        // Attach to the tail (plain store under the lock). has_overflow
        // flag (bit 7) + 1-based idx in bits [8..], occupancy preserved.
        const std::uint64_t cur_hdr = tail->header.load(std::memory_order_acquire);
        const std::uint64_t desired
            = (cur_hdr & 0x7full) | (1ull << 7) | (new_idx << 8);
        tail->header.store(desired, std::memory_order_release);
        viper::internal::pmem_persist(&tail->header, sizeof(tail->header));
        region_at(rid).num_entries.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // -- M3 follow-up: bulk_upsert with amortised PM fence cost --------
    //
    // Same semantics as repeated calls to `upsert`, but groups entries
    // hitting the same bucket and emits ONE sfence per bucket-group
    // (instead of two per entry). Isolation: bulk_upsert takes the
    // per-region lock internally (one publisher per region), so it is
    // safe against concurrent upsert/remove/bulk_upsert on the same
    // region and against lock-free readers. No external mutex required.
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
        std::uint64_t fp;      // routing hash: selects region + bucket
        std::uint64_t key_id;  // full-key identity: match / dedup
        Offset off;
    };

    std::size_t bulk_upsert(std::vector<BulkEntry>& entries) {
        if (entries.empty()) return 0;
        // Sort by (region, bucket) so same-bucket entries cluster.
        // Stable not required; no equal-key duplicates are passed in
        // (HiOM::apply_batch coalesces by (fp64, key_id) before calling).
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
            // Hold the region lock across every bucket-run in this region
            // (entries are sorted region-first, so they're contiguous).
            // Single publisher per region — see upsert(). Uncontended for
            // steady-state flushers (disjoint region ownership).
            std::lock_guard<std::mutex> lk(region_mus_[rid]);
            while (i < entries.size() && region_id_of(entries[i].fp) == rid) {
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
        }
        return total_written;
    }
    // ------------------------------------------------------------------

    // Route by fp64_route, match by full key_id.
    std::optional<Offset> lookup(std::uint64_t fp64_route,
                                 std::uint64_t key_id) const {
        const std::size_t rid = region_id_of(fp64_route);
        const Bucket* cur = &main_bucket_of(rid, fp64_route);
        while (cur != nullptr) {
            // Load the header once: occupancy is the sole visibility
            // authority. A slot whose bit is 0 (empty OR crash residue
            // with key_id+offset durable but header not yet published) is
            // invisible, even if its key_id happens to match. This keeps
            // lookup consistent with scan_chain. Readers are lock-free —
            // the publisher sets the bit last (release), so a matching
            // key_id under a set bit always has a durable entry.
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                if (!slot_published(hdr, i)) continue;
                const std::uint64_t kid = cur->entries[i].key_id.load(
                    std::memory_order_acquire);
                if (kid == key_id) {
                    const std::uint64_t off
                        = cur->entries[i].offset.load(
                              std::memory_order_acquire);
                    if (off == kTombstoneOffset) return std::nullopt;
                    return Offset{off};
                }
            }
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) break;
            cur = overflow_bucket_of(rid, next_idx);
        }
        return std::nullopt;
    }

    bool remove(std::uint64_t fp64_route, std::uint64_t key_id) {
        const std::size_t rid = region_id_of(fp64_route);
        // Writer: serialize on the region so no other publisher mutates
        // this chain concurrently (recovery replay + direct upsert both
        // hit overlapping regions). Readers stay lock-free.
        std::lock_guard<std::mutex> lk(region_mus_[rid]);
        Bucket* cur = &main_bucket_of(rid, fp64_route);
        while (cur != nullptr) {
            const std::uint64_t hdr = cur->header.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                if (!slot_published(hdr, i)) continue;  // only published slots
                const std::uint64_t kid = cur->entries[i].key_id.load(
                    std::memory_order_acquire);
                if (kid == key_id) {
                    cur->entries[i].offset.store(
                        kTombstoneOffset, std::memory_order_release);
                    viper::internal::pmem_persist(
                        &cur->entries[i].offset, sizeof(std::uint64_t));
                    return true;
                }
            }
            const std::uint64_t next_idx = hdr >> 8;
            if (next_idx == kNoOverflow) break;
            cur = overflow_bucket_of(rid, next_idx);
        }
        return false;
    }

    // Region-parallel scan. Each thread takes a strided subset of the
    // 32 regions and applies `visitor(key_id, offset)` to every
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
    // Occupancy bit i of a bucket header is the SOLE authority for slot
    // visibility/reusability (bits 0..6 = per-slot occupancy). A slot is
    // published (visible, update-in-place) iff its bit is 1; bit 0 means
    // free OR crash residue (half-written) — invisible and reusable.
    static bool slot_published(std::uint64_t hdr, std::size_t i) {
        return ((hdr >> i) & 1ull) != 0;
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

    // Apply a contiguous run of `n` BulkEntry that all hash to
    // (rid, bid). Caller (bulk_upsert) already holds region_mus_[rid],
    // so there is a single publisher for this region — no concurrent
    // bulk_upsert / upsert / remove touches the same chain, and
    // lock-free readers gate on the occupancy bit (flipped in Phase 2).
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
        // Bits this batch has already claimed in `b` but not yet flipped
        // into the persisted header (occupancy flip is deferred to Phase
        // 2). A slot is reclaimable only if it is unpublished in the
        // header AND not already claimed earlier in this same batch.
        auto batch_claimed_bits = [&](Bucket* b) -> std::uint64_t {
            for (auto& d : dirty) if (d.b == b) return d.add_bits;
            return 0ull;
        };

        std::size_t written = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint64_t key_id = entries[k].key_id;
            const Offset off = entries[k].off;
            assert(off.offset != kTombstoneOffset);

            // Walk the chain. Occupancy is authoritative:
            //  - published (bit=1 in header, or claimed earlier in this
            //    batch) + key_id match -> update offset in place.
            //  - unpublished + not-batch-claimed -> reusable (empty OR
            //    crash residue); remember the first.
            Bucket* cur = &head;
            Bucket* tail = &head;
            Bucket* found_reusable_b = nullptr;
            std::size_t found_reusable_i = 0;
            bool matched = false;
            while (cur != nullptr) {
                const std::uint64_t hdr
                    = cur->header.load(std::memory_order_acquire);
                const std::uint64_t claimed = batch_claimed_bits(cur);
                const std::uint64_t live = (hdr | claimed) & 0x7full;
                for (std::size_t i = 0; i < kEntriesPerBucket; ++i) {
                    const bool is_live = ((live >> i) & 1ull) != 0;
                    if (is_live) {
                        const std::uint64_t ckid
                            = cur->entries[i].key_id.load(
                                  std::memory_order_acquire);
                        if (ckid == key_id) {
                            // Update path: rewrite the offset slot. Header
                            // bit already set (or will be by this batch);
                            // no new occupancy bit needed.
                            cur->entries[i].offset.store(
                                off.offset, std::memory_order_release);
                            viper::internal::pmem_flush_range(
                                &cur->entries[i].offset,
                                sizeof(std::uint64_t));
                            find_or_create_dirty(cur);
                            matched = true;
                            ++written;
                            break;
                        }
                    } else if (found_reusable_b == nullptr) {
                        found_reusable_b = cur;
                        found_reusable_i = i;
                    }
                }
                if (matched) break;
                tail = cur;
                const std::uint64_t next_idx = hdr >> 8;
                if (next_idx == kNoOverflow) break;
                cur = overflow_bucket_of(rid, next_idx);
            }
            if (matched) continue;

            // Insert path. Claim the first reusable slot (region lock held
            // by bulk_upsert -> single publisher, so plain store, no CAS).
            // The occupancy bit is staged in add_bits and flipped+persisted
            // in Phase 2, AFTER the entry data drain in Phase 1.
            if (found_reusable_b != nullptr) {
                found_reusable_b->entries[found_reusable_i].key_id.store(
                    key_id, std::memory_order_release);
                found_reusable_b->entries[found_reusable_i].offset.store(
                    off.offset, std::memory_order_release);
                viper::internal::pmem_flush_range(
                    &found_reusable_b->entries[found_reusable_i],
                    sizeof(Entry));
                auto& d = find_or_create_dirty(found_reusable_b);
                d.add_bits |= (std::uint64_t{1} << found_reusable_i);
                region_at(rid).num_entries.fetch_add(
                    1, std::memory_order_relaxed);
                ++written;
                continue;
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
            nb->entries[0].key_id.store(key_id, std::memory_order_release);
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
            // Attach: tail->header gets has_overflow (bit 7) + next_idx.
            // Region lock held -> single publisher -> no concurrent
            // extender, so a plain load+store suffices (no CAS loop).
            // Preserve the current occupancy bits; Phase 2's fetch_or
            // layers this batch's add_bits on top, so we don't lose them.
            const std::uint64_t attach_bits = (1ull << 7) | (alloc_idx << 8);
            const std::uint64_t cur_hdr
                = tail->header.load(std::memory_order_acquire);
            tail->header.store((cur_hdr & 0x7full) | attach_bits,
                               std::memory_order_release);
            // Record tail as dirty so Phase 2 flushes its header line
            // (attach bits + any staged occupancy add_bits).
            find_or_create_dirty(tail);
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
                if (((hdr >> i) & 1ull) == 0) continue;  // occupancy authoritative
                const std::uint64_t kid
                    = cur->entries[i].key_id.load(std::memory_order_acquire);
                const std::uint64_t off
                    = cur->entries[i].offset.load(std::memory_order_acquire);
                if (off == kTombstoneOffset) continue;
                visitor(kid, Offset{off});
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
    // Publisher isolation: one writer per region. Writers (upsert /
    // remove / bulk_upsert) lock region_mus_[region_id_of(fp)]; readers
    // never lock. Guarantees no two publishers race to make a bit=0 slot
    // visible, which is what lets occupancy be the sole visibility
    // boundary and lets half-written residue be safely reused/completed.
    std::array<std::mutex, kNumRegions> region_mus_;
};

}  // namespace viper::hiom
