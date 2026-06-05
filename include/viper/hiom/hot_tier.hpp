#pragma once

// HiOM hot tier — compact 8-byte-per-slot DRAM hash table for offset-map caching.
//
// Layout (per §2.7 of design): the table is an array of *buckets*, each
// holding kSlotsPerBucket (= 16) slots in 128 bytes (two contiguous 64-byte
// cache lines, alignas(64)). A parallel metadata array stores per-bucket
// visited bits + hand pointer for SIEVE eviction (M1).
//
// Each slot packs (32-bit fingerprint | 32-bit compact offset) into a single
// 64-bit word so a slot upsert is a single atomic CAS.
//
// fingerprint == 0 is reserved for "empty slot". Callers MUST bias their
// fingerprints to be non-zero (we assert this in debug).
//
// M0 + M1 scope: insert / lookup / remove with last-writer-wins semantics
// on fingerprint collision; SIEVE eviction triggers when a probe window
// is full of distinct fingerprints. No PINNED state yet (M4).
// Concurrency is lock-free via 8-byte atomic CAS on slots and atomic
// fetch_or/and on visited-bit word.
//
// SIEVE algorithm (Yang et al., NSDI '24 — verify before citing):
//   - visited=1 on hit (lookup) and on update.
//   - visited=0 on new insert.
//   - eviction walks slots starting at `hand`: visited=1 → clear and skip;
//     visited=0 → evict.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace viper::hiom {

class HotTier {
  public:
    static constexpr std::size_t kSlotsPerBucket = 16;
    static constexpr std::uint32_t kEmptyFp = 0;
    static constexpr std::uint32_t kInvalidOffset = 0xFFFFFFFFu;
    static constexpr std::size_t kInvalidIdx = static_cast<std::size_t>(-1);

    // Reference to a specific slot, returned by upsert_pinned so the
    // background flusher can later unpin precisely without re-probing.
    // valid==false means the upsert did not place the entry into the
    // hot tier (e.g., bucket full of PINNED slots) — caller should
    // treat the entry as cold-only until the next read warms it.
    struct SlotRef {
        std::size_t bucket_idx{kInvalidIdx};
        std::uint8_t slot_idx{0};
        bool valid{false};
    };

    // 2-bit per-slot state machine (M4 Phase A). Each BucketMeta::state
    // field packs 16 slots × 2 bits into a 32-bit atomic word.
    //
    //   kUnpinned (00) — slot is durable in ColdTier (or empty);
    //                    SIEVE may evict.
    //   kPinned   (01) — upsert_pinned set this; commit-buffer holds
    //                    the corresponding entry, flusher hasn't
    //                    touched yet. SIEVE skips.
    //   kInFlush  (11) — flusher CAS'd ownership and is performing
    //                    the cold-tier write. SIEVE skips.
    //   reserved  (10) — unused.
    //
    // Transitions are the only correctness gate: write-path enters the
    // slot at kPinned (upsert_pinned), flusher transitions kPinned →
    // kInFlush via CAS to take ownership, then kInFlush → kUnpinned
    // after the cold-tier write is durable. Failed CAS is silently
    // skipped — entry's fp64+offset are authoritative for the cold
    // write either way; the slot may have been overwritten by a same-
    // fp update or evicted, in which case "leave its state alone" is
    // the correct behaviour.
    enum class SlotState : std::uint8_t {
        kUnpinned = 0b00,
        kPinned   = 0b01,
        kInFlush  = 0b11,
    };

    // CAS state[ref] from `from` to `to`. Returns true on success.
    // Used by HiOM's flusher to drive the PINNED → IN_FLUSH → UNPINNED
    // transitions; failure means another writer reset the slot or it
    // was evicted, and the caller should leave the state alone.
    bool cas_slot_state(SlotRef ref, SlotState from, SlotState to) {
        if (!ref.valid) return false;
        return cas_state(meta_[ref.bucket_idx], ref.slot_idx, from, to);
    }

    // num_buckets must be a power of two and at least 1.
    // Total slot capacity = num_buckets × kSlotsPerBucket.
    explicit HotTier(std::size_t num_buckets_pow2)
        : num_buckets_(num_buckets_pow2),
          mask_(num_buckets_pow2 - 1),
          buckets_(allocate_buckets(num_buckets_pow2)),
          meta_(num_buckets_pow2) {
        assert((num_buckets_pow2 & (num_buckets_pow2 - 1)) == 0
               && "num_buckets must be a power of two");
        assert(num_buckets_pow2 >= 1);
        assert(reinterpret_cast<std::uintptr_t>(buckets_.get()) % 64 == 0);
    }

    HotTier(const HotTier&) = delete;
    HotTier& operator=(const HotTier&) = delete;

    // Insert or update.
    // - Match (existing fingerprint found): replaces offset, returns OLD offset.
    //   Sets visited bit (the entry is "hot").
    // - New insert (free slot found): returns kInvalidOffset.
    //   Clears visited bit (per SIEVE: new entries start unvisited).
    // - Bucket full of distinct fps: triggers SIEVE eviction; if eviction
    //   yields a slot, claims it as new insert. If eviction fails (e.g.,
    //   future M4 all-PINNED state), returns kInvalidOffset and entry is
    //   NOT stored.
    std::uint32_t upsert(std::uint32_t fingerprint, std::uint32_t offset) {
        assert(fingerprint != kEmptyFp && "fingerprint 0 is reserved for empty");
        const std::size_t bidx = bucket_index(fingerprint);
        Bucket& b = buckets_[bidx];
        BucketMeta& m = meta_[bidx];

        for (int retry = 0; retry < kMaxUpsertRetries; ++retry) {
            std::optional<std::size_t> first_empty;
            bool restart = false;

            for (std::size_t i = 0; i < kSlotsPerBucket; ++i) {
                const std::uint64_t v
                    = b.slots[i].packed.load(std::memory_order_acquire);
                const std::uint32_t cur_fp = unpack_fp(v);

                if (cur_fp == fingerprint) {
                    // Update in place.
                    std::uint64_t expected = v;
                    const std::uint64_t desired = pack(fingerprint, offset);
                    if (b.slots[i].packed.compare_exchange_strong(
                            expected, desired,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        set_visited(m, i);  // update is a "hit"
                        return unpack_off(v);
                    }
                    restart = true;
                    break;
                }
                if (cur_fp == kEmptyFp && !first_empty.has_value()) {
                    first_empty = i;
                }
            }

            if (restart) continue;

            // No matching fp. Try claim first_empty, or evict if bucket full.
            std::size_t target_idx;
            if (first_empty.has_value()) {
                target_idx = *first_empty;
            } else {
                target_idx = sieve_evict(b, m);
                if (target_idx == kInvalidIdx) {
                    // M1: should not happen (no PINNED state). M4: may happen
                    // if every slot is PINNED.
                    return kInvalidOffset;
                }
            }

            std::uint64_t expected = 0;
            const std::uint64_t desired = pack(fingerprint, offset);
            if (b.slots[target_idx].packed.compare_exchange_strong(
                    expected, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                size_.fetch_add(1, std::memory_order_relaxed);
                clear_visited(m, target_idx);  // new entry: unvisited
                return kInvalidOffset;
            }
            // Lost the slot to another writer; restart probe.
        }
        // Pathological retry exhaustion; treat as bucket-full failure.
        return kInvalidOffset;
    }

    // Variant of upsert that PINS the resulting slot. PINNED slots are
    // skipped by SIEVE eviction until unpin() clears the bit. Returns
    // a SlotRef so the caller (typically a background flusher) can
    // later unpin without rehashing. If the bucket is full of PINNED
    // distinct fingerprints, returns SlotRef{valid=false}: the entry
    // is NOT stored, and the caller should rely on the cold tier /
    // commit buffer alone. Stats: pin_failures_ counts these.
    SlotRef upsert_pinned(std::uint32_t fingerprint, std::uint32_t offset) {
        assert(fingerprint != kEmptyFp);
        const std::size_t bidx = bucket_index(fingerprint);
        Bucket& b = buckets_[bidx];
        BucketMeta& m = meta_[bidx];

        for (int retry = 0; retry < kMaxUpsertRetries; ++retry) {
            std::optional<std::size_t> first_empty;
            bool restart = false;

            for (std::size_t i = 0; i < kSlotsPerBucket; ++i) {
                const std::uint64_t v
                    = b.slots[i].packed.load(std::memory_order_acquire);
                const std::uint32_t cur_fp = unpack_fp(v);

                if (cur_fp == fingerprint) {
                    std::uint64_t expected = v;
                    const std::uint64_t desired = pack(fingerprint, offset);
                    if (b.slots[i].packed.compare_exchange_strong(
                            expected, desired,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        set_visited(m, i);
                        force_pinned(m, i);
                        return SlotRef{bidx,
                                       static_cast<std::uint8_t>(i), true};
                    }
                    restart = true;
                    break;
                }
                if (cur_fp == kEmptyFp && !first_empty.has_value()) {
                    first_empty = i;
                }
            }
            if (restart) continue;

            std::size_t target_idx;
            if (first_empty.has_value()) {
                target_idx = *first_empty;
            } else {
                target_idx = sieve_evict(b, m);
                if (target_idx == kInvalidIdx) {
                    pin_failures_.fetch_add(1, std::memory_order_relaxed);
                    return SlotRef{};  // valid=false
                }
            }

            std::uint64_t expected = 0;
            const std::uint64_t desired = pack(fingerprint, offset);
            if (b.slots[target_idx].packed.compare_exchange_strong(
                    expected, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                size_.fetch_add(1, std::memory_order_relaxed);
                clear_visited(m, target_idx);
                force_pinned(m, target_idx);
                return SlotRef{bidx,
                               static_cast<std::uint8_t>(target_idx), true};
            }
            // Lost the slot to another writer; restart probe.
        }
        pin_failures_.fetch_add(1, std::memory_order_relaxed);
        return SlotRef{};
    }

    // Force the slot's state back to kUnpinned, regardless of current
    // state. Most call sites prefer the surgical
    // cas_slot_state(kInFlush, kUnpinned) — this method is for the
    // destructor / shutdown path where we want unconditional release
    // even if a transient race left the slot stuck.
    void unpin(SlotRef ref) {
        if (!ref.valid) return;
        assert(ref.bucket_idx < num_buckets_);
        assert(ref.slot_idx < kSlotsPerBucket);
        force_unpinned(meta_[ref.bucket_idx], ref.slot_idx);
    }

    // Sets visited bit on hit. NOT const because of the side effect.
    // Caller must verify offset → key match (4-byte fp has 1/2^32 collision rate).
    std::optional<std::uint32_t> lookup(std::uint32_t fingerprint) {
        assert(fingerprint != kEmptyFp);
        const std::size_t bidx = bucket_index(fingerprint);
        const Bucket& b = buckets_[bidx];
        for (std::size_t i = 0; i < kSlotsPerBucket; ++i) {
            const std::uint64_t v
                = b.slots[i].packed.load(std::memory_order_acquire);
            const std::uint32_t fp = unpack_fp(v);
            if (fp == fingerprint) {
                set_visited(meta_[bidx], i);
                return unpack_off(v);
            }
            // No early-exit on empty (open-addressing-with-deletion safe).
        }
        return std::nullopt;
    }

    // Const lookup that does NOT set visited (for stats / debugging only).
    std::optional<std::uint32_t> peek(std::uint32_t fingerprint) const {
        assert(fingerprint != kEmptyFp);
        const std::size_t bidx = bucket_index(fingerprint);
        const Bucket& b = buckets_[bidx];
        for (std::size_t i = 0; i < kSlotsPerBucket; ++i) {
            const std::uint64_t v
                = b.slots[i].packed.load(std::memory_order_acquire);
            const std::uint32_t fp = unpack_fp(v);
            if (fp == fingerprint) return unpack_off(v);
        }
        return std::nullopt;
    }

    // Snapshot view of a slot at the address `ref` points at. Used by
    // HiOM::apply_batch for the HotTier-truth winner-picker fast path
    // (M3 follow-up #2 / 2026-05-09): the kPut entry whose packed off
    // matches the slot's current packed off is the canonical writer
    // for this fp32 (the slot was last set by HotTier::upsert_pinned's
    // CAS, which is the linearization point for same-fp32 writes after
    // CCEH retired from the write path in P0). fp == kEmptyFp means
    // the slot was cleared (kRemove or post-eviction). The (fp, off)
    // pair is a snapshot — by the time the caller acts on it another
    // upsert may have CAS-overwritten the slot; that's fine for the
    // picker, which falls back to the alive-and-fp-match walk when no
    // batch entry matches the snapshot.
    struct SlotView {
        std::uint32_t fp;          // kEmptyFp ⇒ slot empty
        std::uint32_t packed_off;  // valid only when fp != kEmptyFp
    };
    SlotView read_slot(SlotRef ref) const {
        if (!ref.valid) return SlotView{kEmptyFp, 0};
        assert(ref.bucket_idx < num_buckets_);
        assert(ref.slot_idx < kSlotsPerBucket);
        const std::uint64_t v = buckets_[ref.bucket_idx]
            .slots[ref.slot_idx]
            .packed.load(std::memory_order_acquire);
        return SlotView{unpack_fp(v), unpack_off(v)};
    }

    // Remove entry by fingerprint. Returns true if removed.
    bool remove(std::uint32_t fingerprint) {
        assert(fingerprint != kEmptyFp);
        const std::size_t bidx = bucket_index(fingerprint);
        Bucket& b = buckets_[bidx];
        for (std::size_t i = 0; i < kSlotsPerBucket; ++i) {
            std::uint64_t expected
                = b.slots[i].packed.load(std::memory_order_acquire);
            if (unpack_fp(expected) == fingerprint) {
                if (b.slots[i].packed.compare_exchange_strong(
                        expected, 0,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    size_.fetch_sub(1, std::memory_order_relaxed);
                    clear_visited(meta_[bidx], i);
                    return true;
                }
                return remove(fingerprint);  // retry
            }
        }
        return false;
    }

    std::size_t num_buckets() const { return num_buckets_; }
    std::size_t capacity() const { return num_buckets_ * kSlotsPerBucket; }
    // White-box DRAM footprint: the 64-byte-aligned bucket array plus the
    // parallel per-bucket SIEVE metadata array. Both scale with num_buckets;
    // sizes are compile-time constants (Bucket=128 B, BucketMeta=8 B).
    std::size_t dram_bytes() const {
        return num_buckets_ * (sizeof(Bucket) + sizeof(BucketMeta));
    }
    std::size_t size() const { return size_.load(std::memory_order_relaxed); }
    std::size_t eviction_count() const {
        return eviction_count_.load(std::memory_order_relaxed);
    }
    std::size_t pin_failures() const {
        return pin_failures_.load(std::memory_order_relaxed);
    }

  private:
    static constexpr int kMaxUpsertRetries = 16;

    struct Slot {
        std::atomic<std::uint64_t> packed{0};
    };
    static_assert(sizeof(Slot) == 8, "Slot must be 8 bytes");

    struct alignas(64) Bucket {
        Slot slots[kSlotsPerBucket];
    };
    static_assert(sizeof(Bucket) == 128, "Bucket must be 128 bytes");
    static_assert(alignof(Bucket) == 64, "Bucket must be 64-byte aligned");

    // Per-bucket SIEVE state. Kept in a parallel array so the slot bucket
    // stays a clean 128B = 2-cache-line probe target.
    //
    // visited: bit i = 1 iff slot i has been hit/updated since last
    //   eviction sweep cleared it.
    // hand: SIEVE hand pointer in [0, 16); next sweep starts here.
    // state: M4 Phase A. 16 × 2-bit per-slot state machine packed into
    //   one 32-bit atomic word; encoding above (SlotState). Replaces
    //   the M3 1-bit `pinned` flag.
    struct alignas(8) BucketMeta {
        std::atomic<std::uint16_t> visited{0};
        std::atomic<std::uint8_t>  hand{0};
        std::uint8_t               pad0{0};
        std::atomic<std::uint32_t> state{0};
    };
    static_assert(sizeof(BucketMeta) == 8, "BucketMeta must be 8 bytes");

    struct BucketArrayDeleter {
        std::size_t count;
        void operator()(Bucket* p) const noexcept {
            if (!p) return;
            for (std::size_t i = 0; i < count; ++i) p[i].~Bucket();
            ::operator delete(p, std::align_val_t{64});
        }
    };
    using BucketArray = std::unique_ptr<Bucket[], BucketArrayDeleter>;

    static BucketArray allocate_buckets(std::size_t n) {
        Bucket* raw = static_cast<Bucket*>(
            ::operator new(sizeof(Bucket) * n, std::align_val_t{64}));
        for (std::size_t i = 0; i < n; ++i) new (raw + i) Bucket{};
        return BucketArray(raw, BucketArrayDeleter{n});
    }

    // SIEVE eviction within a single bucket. Returns the slot index that
    // is now free for the caller to claim, or kInvalidIdx if no slot can
    // be evicted (every non-PINNED slot was visited and we ran out of
    // budget, or every slot is PINNED).
    //
    // PINNED slots are unconditionally skipped: their cold-tier write
    // hasn't completed yet, so dropping them now would create a window
    // where the entry exists nowhere visible to readers.
    //
    // Algorithm: starting at hand, walk up to 2 × kSlotsPerBucket slots.
    //   - if slot is PINNED: skip (don't touch visited bit)
    //   - if slot is empty: return it directly (skipped by some race)
    //   - if visited[i] == 0: clear slot via CAS, advance hand, return i
    //   - if visited[i] == 1: clear visited bit (second-chance) and continue
    // Two passes still suffice when no slots are PINNED.
    std::size_t sieve_evict(Bucket& b, BucketMeta& m) {
        std::uint8_t hand = m.hand.load(std::memory_order_relaxed);
        for (std::size_t k = 0; k < 2 * kSlotsPerBucket; ++k) {
            const std::size_t i = (hand + k) % kSlotsPerBucket;
            // Skip any non-kUnpinned slot (PINNED or IN_FLUSH).
            if (get_state(m, i) != SlotState::kUnpinned) continue;
            const std::uint16_t vis = m.visited.load(std::memory_order_relaxed);
            const bool visited_bit = ((vis >> i) & 1) != 0;
            if (visited_bit) {
                m.visited.fetch_and(
                    static_cast<std::uint16_t>(~(std::uint16_t(1) << i)),
                    std::memory_order_relaxed);
                continue;
            }
            std::uint64_t expected
                = b.slots[i].packed.load(std::memory_order_acquire);
            if (expected == 0) {
                m.hand.store(static_cast<std::uint8_t>((i + 1) % kSlotsPerBucket),
                             std::memory_order_relaxed);
                return i;
            }
            if (b.slots[i].packed.compare_exchange_strong(
                    expected, 0,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                size_.fetch_sub(1, std::memory_order_relaxed);
                eviction_count_.fetch_add(1, std::memory_order_relaxed);
                m.hand.store(static_cast<std::uint8_t>((i + 1) % kSlotsPerBucket),
                             std::memory_order_relaxed);
                return i;
            }
        }
        return kInvalidIdx;
    }

    // visited is a SIEVE hint; correctness is not gated on it (see §2.4 of
    // design). Use relaxed order, and skip the RMW when the bit is already in
    // the desired state — on hit-mostly workloads this turns the steady-state
    // lookup into a pure load + branch.
    static void set_visited(BucketMeta& m, std::size_t slot_idx) {
        const std::uint16_t bit
            = static_cast<std::uint16_t>(std::uint16_t(1) << slot_idx);
        const std::uint16_t cur = m.visited.load(std::memory_order_relaxed);
        if ((cur & bit) != 0) return;
        m.visited.fetch_or(bit, std::memory_order_relaxed);
    }
    static void clear_visited(BucketMeta& m, std::size_t slot_idx) {
        const std::uint16_t bit
            = static_cast<std::uint16_t>(std::uint16_t(1) << slot_idx);
        const std::uint16_t cur = m.visited.load(std::memory_order_relaxed);
        if ((cur & bit) == 0) return;
        m.visited.fetch_and(static_cast<std::uint16_t>(~bit),
                            std::memory_order_relaxed);
    }

    // -- M4 Phase A state machine helpers ------------------------------
    //
    // state[i] occupies bits (2*i) and (2*i+1) of BucketMeta::state.
    // Encoding: see SlotState above.

    static SlotState get_state(const BucketMeta& m, std::size_t i) {
        const std::uint32_t st = m.state.load(std::memory_order_acquire);
        return static_cast<SlotState>((st >> (2 * i)) & 0b11u);
    }

    // CAS state[i] from `from` to `to`. Returns true on success.
    // Loops on benign CAS failure (state changed in another bit-pair);
    // returns false iff state[i] != from when we last looked.
    static bool cas_state(BucketMeta& m, std::size_t i,
                          SlotState from, SlotState to) {
        const std::uint32_t shift = static_cast<std::uint32_t>(2 * i);
        const std::uint32_t mask = static_cast<std::uint32_t>(0b11u) << shift;
        const std::uint32_t from_bits
            = (static_cast<std::uint32_t>(from) & 0b11u) << shift;
        const std::uint32_t to_bits
            = (static_cast<std::uint32_t>(to) & 0b11u) << shift;
        std::uint32_t cur = m.state.load(std::memory_order_acquire);
        while ((cur & mask) == from_bits) {
            const std::uint32_t desired = (cur & ~mask) | to_bits;
            if (m.state.compare_exchange_weak(
                    cur, desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
            // CAS failure can be spurious or due to a race on a
            // different bit-pair; loop continues with the refreshed
            // `cur`. We only return false if state[i] != from.
        }
        return false;
    }

    // Force state[i] to kPinned. Used by upsert_pinned, where the
    // slot was just claimed via CAS on slot.packed; concurrent races
    // (rare: same-fp updates while flusher holds IN_FLUSH) can leave
    // residual kInFlush bits which we clobber here. Implemented as
    // fetch_or + fetch_and on the bit-pair: transient kInFlush is
    // observable but harmless because flusher's CAS(kInFlush,
    // kUnpinned) just no-ops.
    static void force_pinned(BucketMeta& m, std::size_t i) {
        const std::uint32_t shift = static_cast<std::uint32_t>(2 * i);
        const std::uint32_t pin_bit  = std::uint32_t{0b01} << shift;
        const std::uint32_t flush_bit = std::uint32_t{0b10} << shift;
        m.state.fetch_or(pin_bit, std::memory_order_release);
        m.state.fetch_and(~flush_bit, std::memory_order_release);
    }

    // Force state[i] to kUnpinned (clear both bits). Used by
    // HotTier::unpin when the caller wants unconditional release;
    // distinct from cas_state(kInFlush, kUnpinned) which fails if
    // state changed under us.
    static void force_unpinned(BucketMeta& m, std::size_t i) {
        const std::uint32_t shift = static_cast<std::uint32_t>(2 * i);
        const std::uint32_t mask = std::uint32_t{0b11} << shift;
        m.state.fetch_and(~mask, std::memory_order_release);
    }
    // ------------------------------------------------------------------

    static std::uint64_t pack(std::uint32_t fp, std::uint32_t off) {
        return (static_cast<std::uint64_t>(fp) << 32)
               | static_cast<std::uint64_t>(off);
    }
    static std::uint32_t unpack_fp(std::uint64_t v) {
        return static_cast<std::uint32_t>(v >> 32);
    }
    static std::uint32_t unpack_off(std::uint64_t v) {
        return static_cast<std::uint32_t>(v);
    }

    std::size_t bucket_index(std::uint32_t fingerprint) const {
        std::uint64_t h = fingerprint;
        h ^= h >> 16;
        h *= 0x85ebca6b9c1cdcbull;
        h ^= h >> 13;
        return static_cast<std::size_t>(h) & mask_;
    }

    std::size_t num_buckets_;
    std::size_t mask_;
    BucketArray buckets_;
    std::vector<BucketMeta> meta_;
    std::atomic<std::size_t> size_{0};
    std::atomic<std::size_t> eviction_count_{0};
    std::atomic<std::size_t> pin_failures_{0};
};

}  // namespace viper::hiom
