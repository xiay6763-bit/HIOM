#pragma once

// HiOM<K,V> — a tiered offset-map wrapper around viper::Viper<K,V>.
//
// Architecture:
//   - HiOM holds a *reference* to a caller-owned Viper<K,V> and an
//     optional pointer to a caller-owned ColdTier (PM-resident hash
//     index). When ColdTier is attached, write paths mirror every
//     successful op into it; reads consult HotTier → ColdTier → Viper's
//     CCEH (still kept as the M2 safety net).
//   - HotTier (DRAM) caches recently accessed offsets. On lookup, a
//     HotTier hit produces a 4-byte compact offset; we decode it into a
//     full KVOffset, read PM via Viper::ReadOnlyClient::hiom_read_at_offset,
//     and verify the PM-side key matches the lookup key (catches the
//     1/2^32 fp collision case).
//   - On HotTier miss with ColdTier attached: look up the 64-bit fp in
//     ColdTier, decode + PM-verify, and warm HotTier on success.
//   - On both-tier miss we fall through to Viper's normal get() (CCEH).
//     Once M3 retires CCEH this fallback becomes unreachable.
//   - On put/update we go through Viper's existing put/update (so PM
//     persistence ordering is unchanged), peek the installed KVOffset
//     and mirror it into HotTier and (if attached) ColdTier.
//
// What's deferred (later milestones):
//   - Pin / commit-buffer (M3, M4): puts here are not pinned because
//     there is no buffered flush. HotTier eviction is safe to drop a
//     recent insert because the offset is still in ColdTier (or CCEH
//     while M2 keeps it as safety net).
//   - True 32-region routing (M3): every key maps to region 0; the
//     compact offset addresses up to 8K Viper blocks (~192 MB).
//
// Concurrency: each thread should call HiOM::get_client() and use that
// Client object exclusively. HotTier and ColdTier are thread-safe; the
// wrapping Client just forwards.

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "viper/cceh.hpp"
#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/commit_buffer.hpp"
#include "viper/hiom/hot_tier.hpp"
#include "viper/hiom/offset_codec.hpp"

// Read-path hit/miss telemetry. Each HiOM::Client increments its OWN
// per-thread shard (read_shards_[slot_idx_]) — a plain, NON-atomic
// uint64 on a private 64-byte cache line — so the get() hot path emits
// ZERO cross-core coherence traffic. stats() folds the shards into the
// Stats aggregate on demand, off the hot path (fold_read_shards_).
//
// History / why this exists: this used to be a single
// `stats_.<counter>.fetch_add(1, relaxed)` per get(). That one contended
// atomic serialized every read through a single cache line and flattened
// read throughput at >=8 threads — a hard wall at ~12 Mops/s vs Viper's
// ~38 at 24 threads (1M / 100r_uniform). The Step-0 ablation (compiling
// the increment out entirely) restored scaling to 95% of Viper, proving
// the counter was the whole wall; this per-Client shard reproduces that
// win while keeping exact, always-on hit/miss telemetry. See
// design/HIOM.md §7 and Claude memory `hiom-read-stats-contention`.
//
// HIOM_READ_STATS=0 compiles the increment out entirely (no telemetry),
// for absolute-minimal builds or re-running the ablation.
#ifndef HIOM_READ_STATS
#define HIOM_READ_STATS 1
#endif
#if HIOM_READ_STATS
#define HIOM_RSTAT_INC(field)                                  \
    do {                                                       \
        if (slot_idx_ != HiOM::kInvalidSlotIdx)                \
            ++hiom_.read_shards_[slot_idx_].field;             \
    } while (0)
#else
#define HIOM_RSTAT_INC(field) ((void)0)
#endif

// --- HiOM write-path profiling (opt-in, shares VIPER_WRITE_PROFILE) --------
// Times the HiOM-side mirror work (HotTier upsert_pinned, commit-buffer push,
// misc encode/note) so it can be compared against Viper's CCEH index cost.
// The viper-internal phases (incl. the resolver, which replaces CCEH when
// hiom_owns_index_) are captured separately by viper::wprof (viper.hpp).
#ifdef VIPER_WRITE_PROFILE
namespace viper { namespace hiom { namespace wprof2 {
struct Phases {
    std::uint64_t hot_upsert, commit_push, mirror_misc;
    // 2026-07-04 deepening — commit_push split (all inside the old
    // commit_push span): fadd+enqueue vs watermark-wake vs the rare
    // synchronous full drain; and resolver split: HotTier probe (DRAM)
    // vs ColdTier lookup (PM read) vs the key-verify read-at-offset
    // (PM read). Counters let us normalise per-event.
    std::uint64_t push_fadd_enq, push_wake, push_sync_drain;
    std::uint64_t resolv_hot, resolv_cold, resolv_verify;
    std::uint64_t wake_calls, sync_drains, cold_neg_lookups;
};
inline thread_local Phases tl{};
}}}  // namespace viper::hiom::wprof2
#define HWP_DECL() std::uint64_t _hwp = viper::wprof::now()
#define HWP_LAP(f) do { std::uint64_t _n = viper::wprof::now(); viper::hiom::wprof2::tl.f += _n - _hwp; _hwp = _n; } while (0)
#define HWP_CNT(f) (++viper::hiom::wprof2::tl.f)
#else
#define HWP_DECL() ((void)0)
#define HWP_LAP(f) ((void)0)
#define HWP_CNT(f) ((void)0)
#endif

namespace viper::hiom {

// Compute a 4-byte fingerprint from a key. Reuses Viper's routing hash
// (cceh::h, see hash.hpp:63-65) so we do not pay a second hash.
// fp == 0 is reserved by HotTier (kEmptyFp), so we bias to 1.
template <typename K>
inline std::uint32_t key_fingerprint(const K& key) {
    const std::size_t h = cceh::h(&key, sizeof(K));
    const std::uint32_t fp = static_cast<std::uint32_t>(h);
    return fp == 0 ? 1u : fp;
}

// 64-bit version for ColdTier, which uses the top 5 bits to route a key
// to one of 32 regions and the remaining bits as the bucket-id seed.
// Truncating to 32 bits would collapse all keys into region 0 — distinct
// from HotTier, ColdTier needs the full hash. Same bias-to-1 rule
// (fp==0 is ColdTier's empty-slot sentinel).
template <typename K>
inline std::uint64_t key_fingerprint64(const K& key) {
    const std::uint64_t h
        = static_cast<std::uint64_t>(cceh::h(&key, sizeof(K)));
    return h == 0 ? 1ull : h;
}

// Step 1 (HashToken): the write path derives BOTH fingerprints from the
// same key — fp32 for HotTier, fp64 for ColdTier — and the old code called
// cceh::h twice (once per key_fingerprint / key_fingerprint64). fp32 is just
// the low 32 bits of the SAME hash fp64 uses, so a single cceh::h yields
// both. make_key_hash computes it once; fp32/fp64 are bit-identical to what
// the standalone helpers produce (same low-32 / full-64 bias-to-1 rules), so
// HotTier slots and ColdTier entries written via KeyHash match reads that
// still go through key_fingerprint(). Used on the write/remove paths where
// both fingerprints are always needed; the read hot path stays on the
// single-fp32 key_fingerprint() so a hot hit never eagerly computes fp64.
struct KeyHash {
    std::uint32_t fp32;  // HotTier fingerprint (biased away from kEmptyFp)
    std::uint64_t fp64;  // ColdTier fingerprint (biased away from 0)
};

template <typename K>
inline KeyHash make_key_hash(const K& key) {
    const std::uint64_t h
        = static_cast<std::uint64_t>(cceh::h(&key, sizeof(K)));
    const std::uint32_t fp = static_cast<std::uint32_t>(h);
    return KeyHash{fp == 0u ? 1u : fp, h == 0ull ? 1ull : h};
}

template <typename K, typename V>
class HiOM {
  public:
    using ViperT = viper::Viper<K, V>;
    using KVOffset = viper::KeyValueOffset;

    struct Stats {
        std::atomic<std::uint64_t> hot_hits{0};
        std::atomic<std::uint64_t> hot_misses{0};
        std::atomic<std::uint64_t> hot_fp_collisions{0};
        std::atomic<std::uint64_t> hot_warmups{0};
        std::atomic<std::uint64_t> cold_hits{0};
        std::atomic<std::uint64_t> cold_misses{0};
        std::atomic<std::uint64_t> cold_fp_collisions{0};
        // Transient read retries (folded from per-Client shards): gets that
        // spun over a concurrent in-place update's VPage lock rather than
        // failing. Diagnostic; nonzero is healthy.
        std::atomic<std::uint64_t> cold_read_retries{0};
        std::atomic<std::uint64_t> commits_flushed{0};
        std::atomic<std::uint64_t> checkpoints_written{0};
        std::atomic<std::uint64_t> recovery_replayed{0};
        // O(P) stale-lock scan at open: number of durably-locked VPages
        // cleared (residue of a process killed mid in-place update, which
        // locks a page below the durable frontier). Separate from
        // recovery_replayed (the O(tail) suffix replay count).
        std::atomic<std::uint64_t> recovery_locks_cleared{0};
        // Pending-ring backpressure (Claim 5A, bounded producer-consumer
        // window). pending_ring_stalls = number of pending_register calls that
        // had to block because the target ring slot was still occupied by an
        // older, not-yet-retired block (flusher behind); pending_ring_wait_ns =
        // total nanoseconds producers spent waiting on that backpressure. In a
        // healthy run these are ~0; a nonzero stall count in a TIMED phase must
        // be folded into the throughput story (the producer really did wait).
        std::atomic<std::uint64_t> pending_ring_stalls{0};
        std::atomic<std::uint64_t> pending_ring_wait_ns{0};
        // TEMP debug Step 3
        std::atomic<std::uint64_t> debug_flusher_iters{0};
        std::atomic<std::uint64_t> debug_drain_calls{0};
        std::atomic<std::uint64_t> debug_drain_returned_zero{0};
        std::atomic<std::uint64_t> debug_inline_flush_calls{0};
        std::atomic<std::uint64_t> debug_inline_flush_returned_zero{0};
    };

    // Tunables for the background flusher. Defaults are chosen to
    // balance latency (drain in <10 ms steady state) against per-batch
    // overhead. Caller can override at construction.
    //
    // num_flushers (M3 follow-up #2 Step 3, 2026-05-14): how many
    // background flusher threads to spawn, each owning a disjoint
    // subset of CommitBuffer::kNumLanes. Must divide kNumLanes
    // evenly; valid values are 1, 2, 4, 8 (kNumLanes=8). Since same
    // fp64 always lives in one lane, two flushers never touch the
    // same HotTier slot or ColdTier region, so per-flusher mutexes
    // serialize each owner with its own inline-flusher writers
    // without cross-flusher contention.
    //
    // Default 2 (2026-07-09, was 4): background flusher PM writes poison
    // the foreground resolver's PM reads, so fewer flushers => faster
    // writes (all_ops insert t24: 1=1.97 / 2=1.44 / 4=1.31 M ops/s;
    // YCSB-A mixed t24: flat ~13.1 for all). BUT fewer flushers also lag
    // the checkpoint frontier, lengthening the O(tail) recovery replay
    // (integration test: slow flusher => recovery_replayed jumps toward
    // the full write set) — and recovery is HiOM's core (C3) axis. So we
    // do NOT go to 1 (max write, longest tail); 2 keeps most of the write
    // win with a bounded recovery-tail cost. num_flushers=1 stays
    // available as an insert-heavy tuning knob (HIOM_FLUSHERS=1) at the
    // documented cost of a longer recovery tail.
    struct FlusherConfig {
        std::chrono::milliseconds interval{std::chrono::milliseconds(5)};
        std::size_t high_watermark{1024};   // wake on size_hint() >= this
        std::size_t batch_max{8192};        // drain at most this many per cycle
        std::size_t num_flushers{2};
    };

    // Tunables for the M5 A/B checkpoint protocol. cadence_entries is
    // the cumulative-flushed-count distance between checkpoint writes:
    // a checkpoint fires whenever an apply_batch crosses a multiple of
    // it. Smaller -> tighter recovery bound, more PM write traffic;
    // larger -> rarer writes, longer M6 scan window. The default of
    // 4096 trades roughly one checkpoint per ~32 ms of steady-state
    // commit traffic at the M4 default flusher pace.
    struct CheckpointConfig {
        std::uint64_t cadence_entries{4096};
    };

    // M6 tail-scan recovery. tail_scan = true causes the constructor
    // to run a bounded VPage scan from the restored
    // checkpoint.vpage_frontier (or block 0 if no checkpoint) up to
    // viper.hiom_vpage_frontier() and upsert every live record into
    // ColdTier. Idempotent — entries already in cold are overwritten
    // with the same offset, no harm. The scan happens *before* the
    // commit-buffer / flusher are spun up so no concurrent writes
    // interleave with replay. Set tail_scan=false (default) on the
    // create-fresh path; nothing to recover.
    struct RecoveryConfig {
        bool tail_scan{false};
        std::size_t recovery_threads{32};
        // O(P) open-time stale-lock scan. Default OFF since the
        // bounded-lock-set protocol (viper.hpp lock-intent registry):
        // crash-stale VPage locks are repaired lazily on first touch by any
        // reader/writer/replayer, so recovery no longer pays the full-pool
        // page-fault walk (measured 1.6–2.1 s at 100M = 77% of recovery).
        // Keep `true` available as the A/B ablation and belt-and-suspenders
        // mode; both paths satisfy the same lost==0 gate.
        bool stale_lock_scan{false};
    };

    HiOM(ViperT& viper, std::size_t hot_buckets_pow2,
         ColdTier* cold = nullptr,
         FlusherConfig fcfg = FlusherConfig{},
         Checkpoint* checkpoint = nullptr,
         CheckpointConfig ccfg = CheckpointConfig{},
         RecoveryConfig rcfg = RecoveryConfig{})
        : viper_(viper),
          hot_(hot_buckets_pow2),
          cold_(cold),
          base_map_{},  // M0/M2: all zero, single region 0
          fcfg_(fcfg),
          per_lane_high_watermark_(
              std::max<std::size_t>(
                  1, fcfg.high_watermark / CommitBuffer::kNumLanes)),
          checkpoint_(checkpoint),
          ccfg_(ccfg)
    {
        // HiOM does not support Viper block reuse, in ANY mode that
        // attaches a ColdTier: stale ColdTier/HotTier offsets into a
        // reused block break read verification, and the checkpoint
        // frontier proof assumes monotonically growing block numbers
        // (see viper.hpp hiom_reclamation_enabled for the full
        // argument). Refuse loudly rather than corrupt silently.
        if (cold_ != nullptr && viper_.hiom_reclamation_enabled()) {
            std::fprintf(stderr,
                "[HiOM FATAL] ViperConfig::enable_reclamation is on — "
                "HiOM does not support VPage block reuse (breaks offset "
                "verification and the durable checkpoint frontier). "
                "Disable reclamation to attach HiOM.\n");
            std::abort();
        }

        // Step 1: prime cumulative counters from any existing
        // checkpoint so the monotonic invariant survives reopen.
        // read_valid() returns nullopt on a fresh PM file (or one
        // whose only writes were torn) — in that case we start at
        // zero. seq_ is bumped from here on so the next write
        // strictly orders after the last persisted one.
        if (checkpoint_ != nullptr) {
            if (auto rec = checkpoint_->read_valid()) {
                flushed_count_.store(rec->flushed_count,
                                     std::memory_order_relaxed);
                seq_.store(rec->seq, std::memory_order_relaxed);
            }
        }

        // Step 2 (M6): bounded VPage tail scan to bring ColdTier in
        // sync with whatever is on PM past the last checkpoint
        // frontier. Done before the flusher / commit-buffer come up
        // so no concurrent writes interleave with replay.
        if (rcfg.tail_scan && cold_ != nullptr) {
            // Step 1 (correctness, now optional): the O(P) stale-lock scan.
            // Superseded as the default by the bounded-lock-set protocol —
            // every lock acquisition registers intent in a volatile DRAM
            // registry (viper.hpp), so post-crash a locked-but-unregistered
            // page is provably stale and every reader/writer/replayer repairs
            // it lazily on first touch. The full-pool page-fault walk here
            // (measured 1.6–2.1 s @100M, 77% of recovery, fault-bound —
            // threads don't help) is kept behind rcfg.stale_lock_scan as the
            // A/B ablation + belt-and-suspenders mode; both modes satisfy the
            // same lost==0 / mismatch==0 crash gate.
            if (rcfg.stale_lock_scan) {
                const auto ls_start = std::chrono::steady_clock::now();
                const std::size_t locks_cleared
                    = viper_.hiom_clear_all_stale_page_locks(
                        rcfg.recovery_threads);
                const auto ls_end = std::chrono::steady_clock::now();
                recovery_lock_scan_ms_
                    = std::chrono::duration<double, std::milli>(
                          ls_end - ls_start).count();
                stats_.recovery_locks_cleared.fetch_add(
                    locks_cleared, std::memory_order_relaxed);
            }

            // Step 2 (O(tail)): replay the unsafe suffix into ColdTier. Time it
            // in isolation (recovery-sensitivity benchmark, P1). HotTier alloc
            // happens in the init-list above, and viper/cold/checkpoint open
            // before the ctor, so this window is the pure O(tail) replay term.
            // Single-threaded write here (recover_tail_into_cold joins its
            // workers before returning), so a plain double suffices.
            const auto ts_start = std::chrono::steady_clock::now();
            const std::uint64_t replayed
                = recover_tail_into_cold(rcfg.recovery_threads);
            const auto ts_end = std::chrono::steady_clock::now();
            recovery_tail_scan_ms_
                = std::chrono::duration<double, std::milli>(
                      ts_end - ts_start).count();
            stats_.recovery_replayed.fetch_add(
                replayed, std::memory_order_relaxed);
        }

        // Step 3: spin up the steady-state path — commit buffer +
        // flusher thread.
        if (cold_ != nullptr) {
            // M6.5 full: install the old-offset resolver on Viper so
            // its put/update/remove paths can find pre-restart
            // offsets via ColdTier when map_ is empty (the
            // skip_recovery=true case). The resolver fires at most
            // once per key per process — once it returns the
            // pre-restart KVOffset, the write path patches map_ so
            // subsequent ops on the same key skip it entirely.
            // Without ColdTier (M0 mode) we leave the resolver
            // unset so legacy callers stay byte-identical.
            viper_.set_hiom_old_offset_resolver(
                [this](const K& key) -> KVOffset {
                    // M3 follow-up #2 / P0: HiOM owns the index in
                    // owns_index mode, so the resolver is consulted on
                    // every put/update/remove (not just the M6.5 post-
                    // restart fallback). HotTier-first keeps the hit
                    // path on DRAM; ColdTier handles the "HotTier was
                    // evicted but ColdTier holds the prior offset" case
                    // and the post-restart cold-only case.
                    K stored_key{};
                    V stored_val{};
                    HWP_DECL();

                    const std::uint32_t fp32 = key_fingerprint(key);
                    if (auto packed = hot_.lookup(fp32)) {
                        HWP_LAP(resolv_hot);
                        const auto d = decode(*packed,
                                              route_to_region_default(),
                                              base_map_);
                        const KVOffset off{
                            static_cast<viper::block_size_t>(d.block_number),
                            static_cast<viper::page_size_t>(d.page_number),
                            static_cast<viper::data_offset_size_t>(
                                d.data_offset)};
                        auto ro = viper_.get_read_only_client();
                        if (ro.hiom_read_at_offset(off, &stored_key,
                                                   &stored_val)
                            && stored_key == key) {
                            HWP_LAP(resolv_verify);
                            return off;
                        }
                        HWP_LAP(resolv_verify);
                        // fp32 collision (~1/2^32) or stale slot —
                        // fall through to ColdTier.
                    } else {
                        HWP_LAP(resolv_hot);
                    }

                    const std::uint64_t fp64 = key_fingerprint64(key);
                    auto cold_off = cold_->lookup(fp64);
                    HWP_LAP(resolv_cold);
                    if (!cold_off) {
                        HWP_CNT(cold_neg_lookups);
                        return KVOffset::Tombstone();
                    }
                    const KVOffset off = *cold_off;
                    // Verify: fp64 collision rate is ~1 in 2^64 but
                    // ColdTier upserts are coalesced by fp so the
                    // same fp can briefly point at a stale offset
                    // during tombstone GC. Cheap key-verify keeps
                    // semantics identical to map_.Get(key, key_check_fn).
                    auto ro = viper_.get_read_only_client();
                    if (!ro.hiom_read_at_offset(off, &stored_key,
                                                &stored_val)) {
                        HWP_LAP(resolv_verify);
                        return KVOffset::Tombstone();  // page locked / torn
                    }
                    if (!(stored_key == key)) {
                        HWP_LAP(resolv_verify);
                        return KVOffset::Tombstone();  // fp collision
                    }
                    HWP_LAP(resolv_verify);
                    return off;
                });

            // Update fast path: DRAM-only HotTier resolver. Returns the
            // HotTier hit's decoded offset WITHOUT the PM key-verify
            // read the full resolver above does — Viper::Client::update
            // folds that verify into the write lock it must take anyway
            // (read data[slot].first under lock, compare, apply in
            // place), eliminating the separate resolv_verify PM read on
            // the common in-place-update path. hot_.lookup still sets
            // the SIEVE visited bit, so it doubles as the SIEVE touch
            // that Client::update used to do explicitly. A HotTier miss
            // or a candidate that fails the in-lock key-check falls back
            // to the full (verified) resolver installed above.
            viper_.set_hiom_hot_only_resolver(
                [this](const K& key) -> KVOffset {
                    const std::uint32_t fp32 = key_fingerprint(key);
                    if (auto packed = hot_.lookup(fp32)) {
                        const auto d = decode(*packed,
                                              route_to_region_default(),
                                              base_map_);
                        return KVOffset{
                            static_cast<viper::block_size_t>(d.block_number),
                            static_cast<viper::page_size_t>(d.page_number),
                            static_cast<viper::data_offset_size_t>(
                                d.data_offset)};
                    }
                    return KVOffset::Tombstone();
                });

            // M3 follow-up #2 / P0: HiOM is the index from this point
            // on. Viper::Client::put / update / remove see the flag
            // and skip map_.Insert / map_.Get entirely. MUST come
            // after the resolver setter — the put path assumes
            // (owns_index ⇒ resolver installed).
            viper_.set_hiom_owns_index(true);

            commit_buf_ = std::make_unique<CommitBuffer>();
            for (std::size_t i = 0; i < CommitBuffer::kNumLanes; ++i) {
                flusher_consumer_toks_[i]
                    = std::make_unique<moodycamel::ConsumerToken>(
                        commit_buf_->make_consumer_token(i));
            }
            // Step 3: validate and spawn N background flushers, each
            // owning a disjoint lane subset.
            const std::size_t nf = fcfg_.num_flushers;
            if (nf == 0 || (CommitBuffer::kNumLanes % nf) != 0) {
                throw std::runtime_error(
                    "HiOM: num_flushers must divide CommitBuffer::kNumLanes");
            }
            for (std::size_t i = 0; i < nf; ++i) {
                flushers_[i]
                    = std::thread([this, i]() { flusher_loop(i); });
            }
        }
    }

    // Step 3: lane → flusher assignment. Lanes are partitioned in
    // contiguous blocks so each flusher owns a power-of-two run of
    // ColdTier regions; this keeps PM writes from the same flusher
    // confined to a few DIMM channels.
    static std::size_t flusher_of_lane(std::size_t lane_id,
                                       std::size_t num_flushers) {
        const std::size_t lanes_per = CommitBuffer::kNumLanes / num_flushers;
        return lane_id / lanes_per;
    }
    // Bitmask of lanes owned by `flusher_id` under the configured
    // num_flushers. Used by the per-flusher drain to mask the
    // CommitBuffer::nonempty_mask() to its own lanes.
    static std::uint64_t lanes_mask_for_flusher(std::size_t flusher_id,
                                                std::size_t num_flushers) {
        const std::size_t lanes_per = CommitBuffer::kNumLanes / num_flushers;
        const std::size_t start = flusher_id * lanes_per;
        std::uint64_t mask = 0;
        for (std::size_t i = 0; i < lanes_per; ++i) {
            mask |= 1ull << (start + i);
        }
        return mask;
    }

    ~HiOM() {
        // Step 3: signal stop, wake every flusher, then join all that
        // were spawned. The drain_once() catch-up at the end runs
        // single-threaded across all flusher partitions, so any
        // entries left after the threads exited still reach ColdTier.
        if (any_flusher_joinable()) {
            stop_.store(true, std::memory_order_release);
            wake_all_flushers();
            for (auto& t : flushers_) {
                if (t.joinable()) t.join();
            }
            for (std::size_t i = 0; i < fcfg_.num_flushers; ++i) {
                drain_once(i);
            }
        }
    }

    HiOM(const HiOM&) = delete;
    HiOM& operator=(const HiOM&) = delete;

    class Client {
      public:
        // HiOM::get_client() reaches into slot_idx_ to install the
        // M4 Phase E per-client tracking slot just after construction.
        friend class HiOM<K, V>;

        Client(HiOM& hiom, typename ViperT::Client viper_client)
            : hiom_(hiom), viper_(std::move(viper_client)) {}

        // Move ctor: transfer slot ownership so the moved-from instance
        // doesn't release a slot HiOM has assigned to the moved-to one.
        // get_client() relies on guaranteed copy elision (C++17), so
        // the move ctor is rarely invoked, but defensive callers may
        // std::move(client).
        Client(Client&& other) noexcept
            : hiom_(other.hiom_),
              viper_(std::move(other.viper_)),
              prod_toks_(std::move(other.prod_toks_)),
              client_seq_(other.client_seq_),
              slot_idx_(other.slot_idx_) {
            other.slot_idx_ = HiOM::kInvalidSlotIdx;
        }
        Client& operator=(Client&&) = delete;
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        ~Client() {
            if (slot_idx_ != HiOM::kInvalidSlotIdx) {
                hiom_.release_client_slot(slot_idx_);
            }
        }

        bool put(const K& key, const V& value, bool assume_new = false) {
            // Durable-frontier (Claim 5B): publish a conservative in-flight
            // block lower bound BEFORE the VPage write. put lands in this
            // block or a fresh higher one, so it is always <= the block
            // that becomes durable. This closes the window between
            // viper_.put returning durable and mirror_write registering the
            // block: a crash in that window still leaves the frontier <= the
            // just-written block. Only meaningful when a checkpoint is
            // attached (else no frontier). See note_client_block for the
            // post-write update.
            if (slot_idx_ != HiOM::kInvalidSlotIdx
                    && hiom_.checkpoint_ != nullptr) {
                hiom_.note_inflight_block(
                    slot_idx_,
                    static_cast<std::uint64_t>(viper_.hiom_client_block()));
            }
            // Step 1: viper does the PM-side write (data + bitset persisted)
            // and updates its own CCEH. Persistence ordering is unchanged
            // from the original Viper put path (viper.hpp ~1037-1054).
            // Note: viper_.put returns is_new_item (true=insert, false=
            // update of existing key). Both are "success"; only an
            // internal panic would skip the return, so we always run
            // mirror_write below regardless of insert vs. update —
            // otherwise updates of an existing key would silently miss
            // the commit buffer + ColdTier path, leaving the CCEH offset
            // and the HiOM offset divergent (M4 Phase C bug).
            // assume_new (pure-insert fast path): use the public put_assume_new
            // shim so Viper's owns_index branch skips the old-offset resolver
            // (a ColdTier PM negative lookup that is pure waste for a brand-new
            // key). mirror below is unchanged (uses last_put_offset, never the
            // resolver).
            const bool is_new_item = assume_new
                                         ? viper_.put_assume_new(key, value)
                                         : viper_.put(key, value);

            // Step 2: mirror into HotTier (PINNED) and the commit buffer /
            // ColdTier. M4 Phase C: take the offset directly from the
            // Viper Client's last_put_offset() — viper_.put just wrote
            // (key, value) into that exact slot, so it's guaranteed to
            // hold our key. Peeking CCEH (the old approach) could return
            // a stale duplicate entry and lead ColdTier to point at a
            // different key's slot.
            const KVOffset just_written = viper_.last_put_offset();
            mirror_write_with_offset(key, CommitEntry::Op::kPut, just_written);
            return is_new_item;
        }

        bool get(const K& key, V* value) {
            const std::uint32_t fp = key_fingerprint(key);
            if (auto packed = hiom_.hot_.lookup(fp)) {
                if (verify_and_read(key, *packed, value)) {
                    HIOM_RSTAT_INC(hot_hits);
                    return true;
                }
                HIOM_RSTAT_INC(hot_fp_collisions);
            } else {
                HIOM_RSTAT_INC(hot_misses);
            }
            return resolve_slow(key, fp, value);
        }

        // HotTier-miss / fp-collision tail shared by get() and get_batch().
        // `fp` is the caller's already-computed 32-bit fingerprint (kept in
        // the signature so batch pass-2 need not recompute and a future
        // membership-filter hook has it). Behaviour is byte-identical to the
        // original monolithic get()'s fall-through.
        bool resolve_slow(const K& key, std::uint32_t /*fp*/, V* value) {
            if (hiom_.cold_ != nullptr) {
                // M3 Phase D: ColdTier is authoritative when attached.
                // No CCEH fallback. A double-miss (HotTier + ColdTier)
                // returns false even if Viper's CCEH still has the
                // entry — the design contract is that all writes pass
                // through the commit buffer and are eventually visible
                // in ColdTier; reads of recently-written keys must hit
                // the still-PINNED HotTier slot before then. This is
                // the §3 invariant I3 surface (HotTier capacity ≥
                // commit-buffer high watermark).
                const std::uint64_t fp64 = key_fingerprint64(key);
                if (auto cold_off = hiom_.cold_->lookup(fp64)) {
                    if (verify_and_read_offset(key, *cold_off, value)) {
                        HIOM_RSTAT_INC(cold_hits);
                        mirror_into_hot_with_offset(key, *cold_off);
                        return true;
                    }
                    HIOM_RSTAT_INC(cold_fp_collisions);
                } else {
                    HIOM_RSTAT_INC(cold_misses);
                }
                return false;
            }

            // M0/M1 mode (no ColdTier): CCEH is authoritative. This
            // path is exercised by the legacy hit-mostly tests; not
            // the steady-state path once ColdTier is wired up.
            if (!viper_.get(key, value)) return false;
            mirror_into_hot_with_offset(key,
                                        viper_.hiom_peek_offset(key));
            HIOM_RSTAT_INC(hot_warmups);
            return true;
        }

        // Batched group-prefetching get (Chen et al. SIGMOD'04). Resolves
        // `n` independent keys with the PM record-read latencies overlapped:
        //   pass 1 — for every key, probe the DRAM HotTier (fast, no PM) and,
        //            on a hit, decode the compact offset to a KVOffset and
        //            issue a software prefetch for the PM record.
        //   pass 2 — perform the blocking verify+read at each resolved
        //            offset (now warm) and, on a HotTier miss / fp collision,
        //            fall through to the exact same ColdTier path get() uses.
        // Semantics are identical to calling get() on each key in order;
        // found[i] and values[i] mirror get()'s return/out-param. Because
        // HiOM's index is DRAM-resident, pass 1 does zero index-chase PM
        // reads — the two-pass pipeline is a clean value-only prefetch.
        // (NOTE: this is the standard Chen'04 group-prefetch technique, not a
        // HiOM novelty — EEPH+ [TACO'25], whose index is ALSO DRAM-resident,
        // ships the same "batching & prefetching" scheme. get_batch is an
        // engineering reuse, not a contribution; cite Chen'04 + EEPH+.)
        //
        // n must be <= kGetBatchMax. Larger requests are chunked by the
        // caller (fixture) or would overflow the on-stack state array.
        static constexpr std::size_t kGetBatchMax = 32;
        void get_batch(const K* keys, std::size_t n,
                       V* values, bool* found) {
            assert(n <= kGetBatchMax);
            // Per-lookup state carried from pass 1 to pass 2.
            std::uint32_t fp[kGetBatchMax];
            KVOffset off[kGetBatchMax];
            bool hot_hit[kGetBatchMax];

            // Pass 1: DRAM HotTier probe + prefetch resolved PM records.
            for (std::size_t i = 0; i < n; ++i) {
                fp[i] = key_fingerprint(keys[i]);
                hot_hit[i] = false;
                if (auto packed = hiom_.hot_.lookup(fp[i])) {
                    const auto d = decode(*packed, route_to_region_default(),
                                          hiom_.base_map_);
                    off[i] = KVOffset{
                        static_cast<viper::block_size_t>(d.block_number),
                        static_cast<viper::page_size_t>(d.page_number),
                        static_cast<viper::data_offset_size_t>(d.data_offset)};
                    hot_hit[i] = true;
                    if (!off[i].is_tombstone())
                        viper_.hiom_prefetch_at_offset(off[i]);
                }
            }

            // Pass 2: blocking verify+read on the (now warm) records, with
            // the ColdTier fallback matching get() exactly.
            for (std::size_t i = 0; i < n; ++i) {
                if (hot_hit[i]) {
                    if (verify_and_read_offset(keys[i], off[i], &values[i])) {
                        HIOM_RSTAT_INC(hot_hits);
                        found[i] = true;
                        continue;
                    }
                    HIOM_RSTAT_INC(hot_fp_collisions);
                } else {
                    HIOM_RSTAT_INC(hot_misses);
                }
                found[i] = resolve_slow(keys[i], fp[i], &values[i]);
            }
        }

        bool remove(const K& key) {
            const bool viper_ok = viper_.remove(key);
            // Step 1 (HashToken): fp32 (HotTier evict) + fp64 (commit route)
            // from one hash — remove hashed the key twice before.
            const KeyHash kh = make_key_hash(key);
            // M3 follow-up #2 / P0: even if viper_.remove returned false,
            // the resolver may have missed an in-flight kPut for `key`
            // (HotTier fp32 collision + ColdTier stale offset, before
            // the latest put has been flushed). Pushing a kRemove
            // through the buffer regardless makes apply_batch's
            // (fp64,seq) sort the source of truth: the kRemove will be
            // the highest-seq entry for this fp and win the descending
            // walk, evicting the fp from ColdTier. The VPage slot for
            // the latest in-flight put is intentionally leaked in this
            // edge case (we can't find it without scanning the commit
            // buffer); the slot is unreferenced after the kRemove
            // applies and a future reclaim pass would compact it. This
            // is a known P0 trade-off, exercised only when fp32
            // collisions happen during heavy update+remove churn.
            hiom_.hot_.remove(kh.fp32);
            if (hiom_.cold_ != nullptr) {
                // Push remove through the buffer so it serializes with
                // earlier puts of the same key. Direct cold_.remove
                // would race with a still-buffered kPut for this fp,
                // and the flusher would then write the obsolete put
                // *after* the remove.
                push_commit({CommitEntry::Op::kRemove,
                             {}, kh.fp64,
                             KVOffset{},  // unused for kRemove
                             HotTier::SlotRef{},
                             next_seq()});
            }
            // Return value semantics: true means "the cold/hot index
            // no longer references `key` once the buffer drains". We
            // return true whenever a kRemove was pushed, since the
            // P0 fallback may push for a key whose viper_.remove failed
            // due to a transiently-invisible in-flight put. The legacy
            // (no-cold) path keeps the strict "did Viper own this key?"
            // semantics.
            if (hiom_.cold_ == nullptr) return viper_ok;
            return true;
        }

        template <typename UpdateFn>
        bool update(const K& key, UpdateFn fn) {
            // 5B for the RELOCATING path only: a std::string update may
            // move the value to a NEW offset, which is durable before the
            // mirror_write below registers it — the same window put()
            // closes by publishing inflight_block first. Fixed-size V is
            // updated IN-PLACE (offset unchanged, no commit entry pushed),
            // so no bound is needed there — and publishing one anyway
            // would pin the frontier at this block until the client's
            // next put, needlessly inflating the recovery tail.
            if constexpr (std::is_same_v<V, std::string>) {
                if (slot_idx_ != HiOM::kInvalidSlotIdx
                        && hiom_.checkpoint_ != nullptr) {
                    hiom_.note_inflight_block(
                        slot_idx_,
                        static_cast<std::uint64_t>(
                            viper_.hiom_client_block()));
                }
            }
            if (!viper_.update(key, std::move(fn))) return false;
            // Viper's fixed-size update is IN-PLACE: the value is
            // overwritten at the same VPage slot and persisted by
            // viper_.update, so the key's offset is UNCHANGED. ColdTier
            // therefore already maps key→offset correctly and needs no
            // re-write, and there is no new slot to pin — so skip the
            // commit-buffer push entirely and only refresh the HotTier
            // SIEVE "visited" bit (plain hot_.lookup: sets it on hit,
            // no-op on miss; an evicted entry is re-warmed from ColdTier
            // on the next read either way). This removes update's per-op
            // write amplification: the old path did upsert_pinned +
            // push_commit + a redundant flusher ColdTier upsert of the
            // SAME (fp64, offset). Crash-safe — the new value is durable
            // on PM at the unchanged offset and ColdTier's offset is
            // unchanged, so tail-scan recovery resolves the key to the new
            // value with nothing to replay (no commit entry ⇒ no unflushed
            // window ⇒ no checkpoint-frontier constraint, so skipping
            // note_client_block is also safe).
            //
            // Guard: fixed-size V only. A variable-size std::string value
            // can grow and be relocated to a NEW offset by Viper's update,
            // which WOULD need a ColdTier re-write — those keep the
            // mirror_write(kPut) path.
            if constexpr (std::is_same_v<V, std::string>) {
                mirror_write(key, CommitEntry::Op::kPut);
            }
            // Fixed-size V: viper_.update took the owns_index fast path,
            // whose hot_only_resolver already ran hot_.lookup (setting
            // the SIEVE visited bit) to find the slot. No extra touch or
            // commit push needed — the in-place write is durable at the
            // unchanged offset and ColdTier's offset is unchanged.
            return true;
        }

      private:
        // Verify the hot-tier offset really points at `key`'s value, and
        // if so fill *value. Returns false on any mismatch (fp collision,
        // stale offset, locked page, version torn) — caller falls back.
        bool verify_and_read(const K& key, compact_offset_t packed, V* value) {
            const auto d = decode(packed, route_to_region_default(),
                                  hiom_.base_map_);
            const KVOffset off{
                static_cast<viper::block_size_t>(d.block_number),
                static_cast<viper::page_size_t>(d.page_number),
                static_cast<viper::data_offset_size_t>(d.data_offset)};
            return verify_and_read_offset(key, off, value);
        }

        // ColdTier-side verify: KVOffset comes back full-width, no codec.
        // Rides through the read-vs-in-place-update visibility race: a
        // concurrent Client::update takes the VPage version lock for a few
        // instructions, during which hiom_read_at_offset would see the page
        // LOCKED or version-torn. Treating that transient state as a hard miss
        // spuriously failed ~1-in-a-million contended gets under YCSB-A (50%
        // update) — a real read_success_rate < 1.0 with NO true fp64 collision
        // (proven offline). Retry a bounded number of times on the transient
        // status; only a genuine key mismatch or a freed slot is terminal.
        bool verify_and_read_offset(const K& key, KVOffset off, V* value) {
            if (off.is_tombstone()) return false;
            using RS = typename ViperT::ReadOnlyClient::ReadStatus;
            // A page lock is held for only a handful of instructions, so a
            // short bounded spin covers many overlapping updaters without
            // livelocking. The cap is generous (the lock is never held long
            // in steady state); exhausting it means something is genuinely
            // wrong (e.g. a stuck writer), so fall through to a miss rather
            // than spin forever.
            constexpr int kMaxRetries = 1024;
            // Crash-stale repair point: a genuinely-held lock releases within
            // nanoseconds, so hitting a quarter of the budget on the SAME
            // offset is decisive evidence of a lock left by a killed process
            // (post-crash the intent registry is empty ⇒ repair succeeds and
            // the next attempt reads normally). Replaces the O(P) open-time
            // lock scan as the correctness mechanism; see viper.hpp's
            // bounded-lock-set protocol block.
            constexpr int kRepairAttempt = kMaxRetries / 4;
            K pm_key;
            V pm_val;
            for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
                const RS st =
                    viper_.hiom_read_at_offset_status(off, &pm_key, &pm_val);
                if (st == RS::kOk) {
                    if (!(pm_key == key)) return false;  // genuine fp collision
                    *value = std::move(pm_val);
                    return true;
                }
                if (st == RS::kSlotFree) return false;   // terminal: freed slot
                // kTransient: a concurrent in-place update holds the lock or
                // bumped the version mid-read. Pause briefly and retry; the
                // lock is released within a few instructions, so a single
                // pause per attempt suffices to avoid hammering the cacheline.
                HIOM_RSTAT_INC(cold_read_retries);
                if (attempt == kRepairAttempt) {
                    viper_.hiom_repair_stale_lock_at(off);
                }
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            }
            return false;  // retries exhausted — treat as miss (surfaced by gate)
        }

        // Look up the current KVOffset for `key` via Viper's CCEH and
        // route it into HotTier (PINNED if buffer attached) + commit
        // buffer / ColdTier. The HotTier mirror is best-effort (silently
        // skips when the block is out of range for M0's degenerate
        // 1-region map; ColdTier still has it, so correctness holds).
        void mirror_write(const K& key, CommitEntry::Op op) {
            KVOffset off = viper_.hiom_peek_offset(key);
            if (off.is_tombstone()) return;
            mirror_write_with_offset(key, op, off);
        }

        // M4 Phase C: same as mirror_write, but caller supplies the
        // KVOffset directly (e.g., from viper_.last_put_offset() on
        // the put hot path). Avoids a CCEH peek that under heavy
        // multi-producer same-key concurrency can return a stale
        // duplicate offset whose VPage slot has been re-allocated to
        // a different key — exactly the bug Phase C is meant to fix.
        void mirror_write_with_offset(const K& key, CommitEntry::Op op,
                                      KVOffset off) {
            if (off.is_tombstone()) return;
            HWP_DECL();

            // Step 1 (HashToken): one cceh::h for both fingerprints. fp32
            // (HotTier pin) and fp64 (ColdTier route + commit entry) are the
            // low-32 / full-64 slices of the same hash — computing them apart
            // hashed the key twice per write.
            const KeyHash kh = make_key_hash(key);

            // M4 Phase E: report this client's most recent write block
            // so HiOM's next checkpoint frontier can be computed as
            // the min over all active clients. Without this, an "old"
            // client still writing into a low-numbered block while the
            // global current_block_page_ has advanced will be missed
            // by tail-scan recovery's [frontier-1, current) range, and
            // its yet-to-flush commit-buffer entries become unrecoverable
            // on a crash. Done here (not in push_commit) so it covers
            // both kPut and kRemove paths uniformly — kRemove targets
            // the same block the client is currently writing into.
            if (slot_idx_ != HiOM::kInvalidSlotIdx) {
                hiom_.note_client_block(
                    slot_idx_,
                    static_cast<std::uint64_t>(off.block_number));
            }

            HotTier::SlotRef ref{};
            const auto [block, page, slot] = off.get_offsets();
            auto packed = encode(block,
                                 static_cast<std::uint8_t>(page),
                                 static_cast<std::uint16_t>(slot),
                                 route_to_region_default(),
                                 hiom_.base_map_);
            // `packed_ok` is the "we actually got to call upsert_pinned"
            // gate. Two reasons it can be false:
            //   1. encode() returned nullopt (block_number > 13-bit
            //      ceiling = 8191 — fires once Viper has allocated >8K
            //      VPageBlocks; with KeyType16+ValueType200 that's at
            //      ~885K records and is then permanent for the rest of
            //      the run);
            //   2. cold_ is null (M0 mode, no buffered path).
            // Only case (1) keeps growing — in (1) we did NOT pin a
            // HotTier slot, but the put has already landed in Viper
            // PMem and push_commit will route it through the commit
            // buffer into ColdTier. Reads of this key fall through to
            // ColdTier (HotTier-truth misses, ColdTier authoritative
            // per M3 Phase D). No need to *synchronously* drain the
            // buffer for correctness; the background flusher handles
            // it. The blocking drain below was sized for the M4
            // Phase B "bucket all-PINNED" back-pressure case (rare,
            // per the original comment) and triggering it on encode
            // failure stalled the producer ~50 µs per put,
            // collapsing thread:1 insert throughput to ~18 K/s. Gate
            // it on `packed_ok` so encode failure short-circuits.
            HWP_LAP(mirror_misc);
            const bool packed_ok = packed.has_value();
            if (packed_ok) {
                if (hiom_.cold_ != nullptr) {
                    // Buffered path: pin so the slot survives until
                    // the flusher writes the corresponding cold-tier
                    // entry. M4 Phase B: if the bucket is full of
                    // PINNED slots, drain a chunk synchronously and
                    // retry — the inline-flush turns some PINNED
                    // slots back to UNPINNED, freeing room.
                    const std::uint32_t fp32 = kh.fp32;
                    ref = hiom_.hot_.upsert_pinned(fp32, *packed);
                    for (int attempt = 0;
                         !ref.valid && attempt < kMaxBackpressureRetries;
                         ++attempt) {
                        if (hiom_.try_inline_flush(kInlineFlushBatch) == 0) {
                            // Another thread is draining (or queue
                            // empty); nudge background flushers and
                            // yield briefly.
                            hiom_.wake_all_flushers();
                            std::this_thread::sleep_for(
                                std::chrono::microseconds(50));
                        }
                        ref = hiom_.hot_.upsert_pinned(fp32, *packed);
                    }
                } else {
                    // No buffer; classic M0 mirror. Eviction is safe
                    // because Viper's CCEH still holds the offset.
                    hiom_.hot_.upsert(kh.fp32, *packed);
                }
            }
            HWP_LAP(hot_upsert);

            if (hiom_.cold_ != nullptr) {
                push_commit({op, {}, kh.fp64, off, ref,
                             next_seq()});
                // NOTE (profiling): commit_push below is the wall-clock of
                // the push_commit call; its internal fadd+enqueue / wake
                // split is accumulated separately (push_fadd_enq,
                // push_wake) inside push_commit — components, not
                // additional time.
                HWP_LAP(commit_push);
                // M4 Phase B: if upsert_pinned failed (ref.valid==false),
                // the entry isn't in HotTier. Block until the commit
                // buffer is drained so it reaches ColdTier before this
                // put returns — otherwise reads of `key` during the
                // commit window would miss both tiers (CCEH retired
                // in M3 Phase D). Synchronous; only fires on the rare
                // bucket-full path.
                //
                // Gate: only when we ACTUALLY tried upsert_pinned and it
                // returned invalid (real back-pressure). Encode failure
                // is not back-pressure — the put already landed in
                // Viper PMem and push_commit routes it to ColdTier
                // asynchronously; reads fall through to ColdTier just
                // fine. Without `packed_ok`, encode failure (common
                // once block_number > 8191 with larger values, e.g.
                // KeyType16+ValueType200 at ~885K records) would block
                // the producer on a full drain every single put.
                if (packed_ok && !ref.valid && hiom_.commit_buf_) {
                    HWP_CNT(sync_drains);
                    while (hiom_.commit_buf_->size_hint() > 0) {
                        if (hiom_.try_inline_flush(kInlineFlushBatch) == 0) {
                            hiom_.wake_all_flushers();
                            std::this_thread::sleep_for(
                                std::chrono::microseconds(50));
                        }
                    }
                }
                HWP_LAP(push_sync_drain);
            } else {
                HWP_LAP(commit_push);
            }
        }

        static constexpr int kMaxBackpressureRetries = 32;
        static constexpr std::size_t kInlineFlushBatch = 256;

        // Encode the supplied full KVOffset and stuff into HotTier.
        // Skips silently if the block is out of range for M0's degenerate
        // 1-region codec.
        void mirror_into_hot_with_offset(const K& key, KVOffset off) {
            const auto [block, page, slot] = off.get_offsets();
            auto packed = encode(block,
                                 static_cast<std::uint8_t>(page),
                                 static_cast<std::uint16_t>(slot),
                                 route_to_region_default(),
                                 hiom_.base_map_);
            if (!packed) return;
            hiom_.hot_.upsert(key_fingerprint(key), *packed);
        }

        // Push a CommitEntry into the buffer using this Client's
        // per-(thread, lane) ProducerToken (allocated lazily on first
        // push to that lane). Token allocation is the only contention
        // point with the queue here; subsequent pushes are near-SPSC.
        // Routing: same fp64 always maps to the same lane, so all
        // entries for a given key end up in the same drained batch
        // and the (fp64, seq) sort + winner picker stays correct.
        void push_commit(const CommitEntry& e) {
            HWP_DECL();
            const std::size_t lane = CommitBuffer::lane_of_fp64(e.fp64);
            if (!prod_toks_[lane]) {
                prod_toks_[lane]
                    = std::make_unique<moodycamel::ProducerToken>(
                        hiom_.commit_buf_->make_producer_token(lane));
            }
            // Durable-frontier tracking (Claim 5A): register this entry's
            // block as pending-unflushed BEFORE it enters the buffer, so a
            // checkpoint can never publish a frontier past a block that
            // still has buffered (crash-losable) entries. Only needed when
            // a checkpoint is attached (else there is no frontier to
            // protect). kRemove entries carry a dummy/tombstone offset (no
            // real VPage block to protect — a remove's recovery is handled
            // by the freed VPage slot), so only kPut with a real offset is
            // tracked. Retired after the ColdTier drain in apply_batch.
            if (hiom_.checkpoint_ != nullptr
                    && e.op == CommitEntry::Op::kPut
                    && !e.off.is_tombstone()) {
                hiom_.pending_register(
                    static_cast<std::uint64_t>(e.off.block_number));
            }
            // push() returns this lane's post-enqueue depth. Wake the
            // flushers on the RISING EDGE through a per-lane watermark
            // (depth == high_watermark/kNumLanes), and ONLY that edge.
            // Two failure modes this threads between:
            //   - depth>=wm (wake on every push once at/over the mark):
            //     under a write storm the flushers fall behind, the lane
            //     sits above the mark, and EVERY push then runs
            //     wake_all_flushers (kNumLanes mutexes) — that 8-mutex
            //     storm regressed delete throughput 8→24 threads.
            //   - depth==1 (wake on every empty→nonempty edge): at low
            //     concurrency the flusher drains each entry and re-parks,
            //     so the producer's next push re-arms the edge and pays a
            //     futex wake (syscall) PER op — collapsed delete t=1 to
            //     ~0.28 M/s.
            // The exact-watermark edge fires at most once per drain-cycle:
            // at t=1 the depth stays below wm (flusher keeps up) so we
            // never wake and the 5 ms flusher timer drains the trickle
            // (matches the original size_hint() semantics); under load the
            // depth crosses wm once and we wake once. A missed edge (rare,
            // concurrent drain) costs at most one fcfg_.interval of latency
            // — the flusher's timer + predicate re-check is the backstop.
            const std::size_t lane_depth
                = hiom_.commit_buf_->push(*prod_toks_[lane], lane, e);
            HWP_LAP(push_fadd_enq);
            if (lane_depth == hiom_.per_lane_high_watermark_) {
                hiom_.wake_all_flushers();
                HWP_CNT(wake_calls);
            }
            HWP_LAP(push_wake);
        }

        // M3 follow-up #2 Step 1 (2026-05-14): per-Client local seq,
        // packed with slot_idx as a low-16-bit tiebreaker. Pre-increment
        // so the first emitted seq is (1 << 16) = 65536, well clear of
        // the CommitEntry's default-constructed sentinel (seq == 0).
        //
        // Layout (high to low): [ 48 bits client_local_seq | 16 bits slot_idx ].
        // - intra-Client: client_local_seq is strictly monotone per push,
        //   so the high 48 bits give strict per-Client ordering.
        // - cross-Client tiebreaker: slot_idx in the low 16 bits is the
        //   final resort when two Clients happen to emit the same
        //   client_local_seq for entries that collide on fp64. This is
        //   only consulted by apply_batch's fallback walk (HotTier-truth
        //   fast path doesn't depend on seq); the bias toward higher
        //   slot_idx is acceptable because the case is rare and the
        //   walk only needs *some* deterministic winner.
        // slot_idx_ may be kInvalidSlotIdx if the reservation pool was
        // exhausted (>256 concurrent Clients); masking with 0xFFFF
        // collapses that case to all-ones, which is fine (degenerate
        // tiebreaker; correctness via HotTier-truth path is preserved).
        std::uint64_t next_seq() {
            const std::uint64_t local = ++client_seq_;
            return (local << 16)
                 | (static_cast<std::uint64_t>(slot_idx_) & 0xFFFFu);
        }

        HiOM& hiom_;
        typename ViperT::Client viper_;
        std::array<std::unique_ptr<moodycamel::ProducerToken>,
                   CommitBuffer::kNumLanes> prod_toks_;
        std::uint64_t client_seq_{0};
        // M4 Phase E (multi-writer-aware tail-scan frontier): each
        // Client reserves a slot in HiOM's client_slots_ array on
        // construction and updates client_slots_[slot_idx_].last_block
        // after every successful viper_.put. Released in the dtor.
        // kInvalidSlotIdx (== max) means "no tracking" (slot reservation
        // exhausted, or moved-from instance whose slot was transferred).
        std::size_t slot_idx_{kInvalidSlotIdx};
    };

    static constexpr std::size_t kInvalidSlotIdx
        = std::numeric_limits<std::size_t>::max();

    Client get_client() {
        Client c{*this, viper_.get_client()};
        c.slot_idx_ = reserve_client_slot();
        // Fail-fast (Claim 5B): with a checkpoint attached, an untracked
        // client (no slot) could write into blocks the frontier then
        // advances past, silently losing those records on a crash. The
        // slot pool (kMaxClientSlots=256) is far above any real per-process
        // thread count, so exhaustion means a Client leak, not legitimate
        // load — abort rather than run unsafe.
        if (c.slot_idx_ == kInvalidSlotIdx && checkpoint_ != nullptr) {
            std::fprintf(stderr,
                "[HiOM FATAL] client slot pool exhausted (%zu) with a "
                "checkpoint attached — cannot track the durable frontier "
                "safely; likely a Client leak\n",
                kMaxClientSlots);
            std::abort();
        }
        return c;
    }

    HotTier& hot_tier() { return hot_; }
    ColdTier* cold_tier() { return cold_; }
    CommitBuffer* commit_buffer() { return commit_buf_.get(); }
    // Non-const: folds the per-Client read shards into the aggregate
    // before returning (see fold_read_shards_), so the read-path
    // counters (hot/cold hits/misses/fp_collisions, hot_warmups) reflect
    // every Client's shard. Flusher/recovery counters (commits_flushed,
    // checkpoints_written, recovery_replayed, debug_*) are written
    // directly elsewhere and pass through the fold untouched. Call at a
    // point quiescent w.r.t. the read path (all current callers do).
    const Stats& stats() {
        fold_read_shards_();
        return stats_;
    }

    // Wall-clock of the M6 tail-scan replay (recover_tail_into_cold) in
    // isolation, set once by the ctor when tail_scan=true (else stays 0).
    // The recovery-sensitivity benchmark plots this as the pure O(tail) cost,
    // separate from fixed open overhead (HotTier alloc + viper/cold/chkpt
    // open, all outside this window).
    double recovery_tail_scan_ms() const { return recovery_tail_scan_ms_; }

    // Wall-clock of the O(P) stale-lock scan (hiom_clear_all_stale_page_locks)
    // over all used VPages, set once by the ctor when tail_scan=true. Reported
    // alongside recovery_tail_scan_ms so the end-to-end recovery cost is
    // stated as O(P) lock scan + O(U) replay, not pure O(unsafe suffix).
    double recovery_lock_scan_ms() const { return recovery_lock_scan_ms_; }

    // White-box DRAM footprint of the HiOM index tiers. HotTier (DRAM hash
    // table) is the dominant term; ColdTier lives in a PMem mmap (≈0 DRAM);
    // the commit buffer is transient (drained between flushes) and excluded.
    // Used by HiOMFixture::fixture_dram_bytes() for the index-DRAM metric.
    std::size_t dram_bytes() const {
        std::size_t b = hot_.dram_bytes();
        if (cold_ != nullptr) b += cold_->dram_bytes();
        return b;
    }
    BlockBaseMap& base_map() { return base_map_; }

    // Warm the DRAM HotTier from the authoritative ColdTier: install every
    // live (fp64, offset) into HotTier, touching its pages and populating
    // the offset cache. OFF the steady-state path — intended for benchmark
    // fixtures (gated by HIOM_WARM_HOT) to lift the lazy-mmap first-touch
    // page-fault + the cold→hot re-warm OUT of the timed read loop, so the
    // measured phase reflects steady-state HotTier hits rather than the
    // one-shot warm-up (see the lazy-mmap change in hot_tier.hpp).
    //
    // Returns the number of offsets installed. Best-effort: an entry whose
    // offset is not encodable into the 4-byte compact form (block beyond
    // the single-region codec range, §4.4) is skipped — it stays
    // ColdTier-only exactly as in steady state, and a read of that key
    // simply misses HotTier and resolves via ColdTier.
    //
    // Concurrency: call when no client is writing (post-prefill /
    // pre-timed-loop). HotTier::upsert is lock-free, so the parallel_load
    // workers are safe; under a HotTier smaller than the live set they may
    // evict each other and the surviving residents are arbitrary — warming
    // is only meaningful at HotTier capacity ≥ working set (the full-
    // capacity read config), which is exactly where the warm-up artifact
    // appears. The C2 from-empty capacity sweep deliberately does NOT set
    // HIOM_WARM_HOT, so it keeps warming during the read phase as before.
    std::size_t warm_hot_tier(std::size_t num_threads = 8) {
        if (cold_ == nullptr) return 0;
        std::atomic<std::size_t> warmed{0};
        cold_->parallel_load(
            num_threads,
            [this, &warmed](std::uint64_t fp64, KVOffset off) {
                if (off.is_tombstone()) return;
                const auto [block, page, slot] = off.get_offsets();
                auto packed = encode(
                    block, static_cast<std::uint8_t>(page),
                    static_cast<std::uint16_t>(slot),
                    route_to_region_default(), base_map_);
                if (!packed) return;
                // Reconstruct the fp32 key_fingerprint() would have produced
                // (low 32 bits of the same hash, biased away from kEmptyFp).
                std::uint32_t fp32 = static_cast<std::uint32_t>(fp64);
                if (fp32 == HotTier::kEmptyFp) fp32 = 1u;
                hot_.upsert(fp32, *packed);
                warmed.fetch_add(1, std::memory_order_relaxed);
            });
        return warmed.load(std::memory_order_relaxed);
    }

    // M5 introspection. flushed_count is the cumulative number of
    // commit-buffer entries that have been applied to ColdTier (across
    // restarts, primed from any restored checkpoint). checkpoint()
    // returns the attached PM-resident checkpoint object (or nullptr).
    std::uint64_t flushed_count() const {
        return flushed_count_.load(std::memory_order_acquire);
    }
    Checkpoint* checkpoint() const { return checkpoint_; }
    // Force a checkpoint write outside of cadence boundaries; used by
    // tests and by graceful shutdown to seal the latest state.
    void force_checkpoint() {
        if (checkpoint_ != nullptr) try_write_checkpoint();
    }

    // M6 test hook: simulate a crash by stopping the flusher and
    // dropping the commit buffer *without* draining. After this call
    // the destructor is a no-op for the buffer (drain_once short-
    // circuits on null commit_buf_), so any in-flight commit entries
    // are lost — exactly the volatile-state-loss semantics a real
    // process kill would produce. ColdTier and Checkpoint are
    // untouched (PM-resident, durable per-op). Test-only — production
    // shutdown should call flush_and_wait + force_checkpoint instead.
    void simulate_crash_for_test() {
        if (any_flusher_joinable()) {
            stop_.store(true, std::memory_order_release);
            wake_all_flushers();
            for (auto& t : flushers_) {
                if (t.joinable()) t.join();
            }
        }
        commit_buf_.reset();
    }

    // Test hooks for the bounded pending-ring backpressure (Claim 5A). These
    // drive pending_register/pending_retire directly so a unit test can force a
    // residue collision deterministically — reproducing the "flusher fell
    // >= kWindow behind" condition in-process without a multi-GiB insert storm
    // whose exact scheduling is hard to pin. Test-only; not on any steady path.
    void pending_register_for_test(std::uint64_t block) { pending_register(block); }
    void pending_retire_for_test(std::uint64_t block) { pending_retire(block); }
    static constexpr std::size_t pending_window_for_test() {
        return PendingBlocks::kWindow;
    }
    std::uint64_t pending_ring_stalls_for_test() const {
        return stats_.pending_ring_stalls.load(std::memory_order_relaxed);
    }

    // Block until the commit buffer is fully drained (all pending
    // writes have been applied to ColdTier and corresponding HotTier
    // pins released). Used by tests and graceful shutdown sequences;
    // not on the steady-state path.
    void flush_and_wait() {
        if (!commit_buf_) return;
        while (true) {
            wake_all_flushers();
            // Caught up only when the queue is empty AND no flusher is
            // mid-batch. Both must be observed clear in sequence; a
            // producer may push between them, so we recheck a few
            // times before declaring stable.
            if (commit_buf_->size_hint() == 0 && !any_flushing()) {
                bool stable = true;
                for (int i = 0; i < 4; ++i) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    if (commit_buf_->size_hint() != 0 || any_flushing()) {
                        stable = false;
                        break;
                    }
                }
                if (stable) return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // M4 Phase B + Step 3 cooperative inline-flush: drain entries from
    // the commit buffer synchronously on the calling thread when
    // HotTier upsert hits a bucket-full-of-PINNED case. Step 3
    // change: instead of holding a single global apply_mu_, try to
    // grab any free flusher's mutex and drain that flusher's lanes.
    // This keeps inline-flush from blocking on a single contended
    // mutex when multiple writers hit back-pressure at once; only
    // flushers whose mu is uncontended actually do work, which is
    // exactly the cooperative pattern (writer helps idle flushers,
    // backs off when background flushers are busy keeping up).
    //
    // Returns total drained across whichever flushers we acquired
    // (always at most one in this implementation — caller spins and
    // retries to drain more if needed).
    std::size_t try_inline_flush(std::size_t /*target ignored after Phase C*/) {
        if (!commit_buf_ || cold_ == nullptr) return 0;
        stats_.debug_inline_flush_calls.fetch_add(1, std::memory_order_relaxed);
        const std::size_t nf = fcfg_.num_flushers;
        for (std::size_t f = 0; f < nf; ++f) {
            std::unique_lock<std::mutex> lk(flusher_mus_[f],
                                            std::try_to_lock);
            if (lk.owns_lock()) {
                const std::size_t got
                    = drain_to_empty_and_apply_locked(f);
                if (got == 0) {
                    stats_.debug_inline_flush_returned_zero.fetch_add(
                        1, std::memory_order_relaxed);
                }
                return got;
            }
        }
        stats_.debug_inline_flush_returned_zero.fetch_add(
            1, std::memory_order_relaxed);
        return 0;
    }

  private:
    // -- M4 Phase E: per-client block tracking ---------------------
    //
    // Each HiOM::Client reserves one slot of client_slots_ on
    // construction (via reserve_client_slot, called from get_client)
    // and updates client_slots_[idx].last_block on every successful
    // mirror_write. The dtor releases the slot. min_active_writer_block
    // walks the array and returns the smallest last_block over all
    // currently-active slots (or kInvalidBlock if none) — used by
    // try_write_checkpoint to pick a frontier that's a safe lower
    // bound for tail-scan recovery, even with multiple writers
    // concurrently filling different VPageBlocks.
    //
    // Why a fixed array of slots rather than a std::vector<Client*>:
    // - The hot path is `note_client_block` (every cl.put). A
    //   contention-free atomic write to client_slots_[idx].last_block
    //   is the right cost.
    // - The aggregation path is min_active_writer_block (only on
    //   try_write_checkpoint, ~ms cadence). Walking 256 slots with
    //   atomic loads is negligible.
    // - kMaxClientSlots = 256 is far above realistic per-process
    //   thread counts; reservation appends a "no tracking" fallback
    //   if exhausted (correctness preserved via global frontier).
    static constexpr std::size_t kMaxClientSlots = 256;
    static constexpr std::uint64_t kNoActiveBlock
        = std::numeric_limits<std::uint64_t>::max();

    struct alignas(64) ClientSlot {
        // last_block: the block of this client's most recent completed
        // mirror_write. inflight_block: a conservative lower bound on the
        // block this client is ABOUT to write, published BEFORE viper_.put
        // so a client killed between the durable VPage write and the
        // commit-buffer register still constrains the checkpoint frontier
        // (5B). The frontier uses min(last_block, inflight_block) per slot.
        std::atomic<std::uint64_t> last_block{kNoActiveBlock};
        std::atomic<std::uint64_t> inflight_block{kNoActiveBlock};
        std::atomic<bool> active{false};
    };
    std::array<ClientSlot, kMaxClientSlots> client_slots_{};

    // -- Durable-frontier pending-block tracker (Claim 5A, v2) ----------
    //
    // Tracks, per VPage block, how many commit-buffer entries reference it
    // that are NOT yet durably applied to ColdTier. A checkpoint frontier
    // must never advance past the earliest such block, or a crash (which
    // drops the DRAM buffer) loses those acknowledged records — the P0 bug
    // the SIGKILL test caught. register on push_commit, retire after the
    // ColdTier bulk_upsert drain returns durable.
    //
    // v2 design — tagged-slot ring (an exact bounded multiset). The v1
    // modulo-count ring (count[b % W] + a racy advancing `base`) had no
    // slot identity: a register below base was lost forever, base could
    // advance over an in-progress register, generations aliased after
    // wrap, and reopen mis-initialised base to 0. All of those were one
    // root cause — the ring forgot WHICH block a count belonged to.
    //
    // Here every slot is a single packed atomic u64:
    //
    //     [63:32] block  — the ABSOLUTE block number this slot tracks
    //     [31:0]  count  — pending (registered, un-retired) entries
    //
    // count==0 means the slot is FREE (block field is then meaningless).
    // All transitions are CAS-validated against the full packed word, so
    // a count can never be attributed to the wrong block:
    //   register(b): free slot        -> claim {b, 1}
    //                slot tagged b    -> {b, count+1}
    //                slot tagged b'≠b -> two live blocks with the same
    //                                    residue mod kWindow alias one
    //                                    slot -> fail fast. NOTE what
    //                                    this does and does NOT bound:
    //                                    the ring tracks at most kWindow
    //                                    DISTINCT pending blocks and
    //                                    aborts on residue collision; it
    //                                    does NOT cap the SPREAD
    //                                    max−min of pending blocks
    //                                    (blocks 0 and kWindow+1 occupy
    //                                    slots 0 and 1 — no collision,
    //                                    spread > window). So the unsafe
    //                                    suffix / recovery scan length
    //                                    is frontier − earliest_pending:
    //                                    O(actual backlog), empirically
    //                                    small, but NOT hard-capped at
    //                                    kWindow blocks.
    //   retire(b):   slot must be tagged b with count>0 (else a fatal
    //                accounting bug), -> {b, count-1}; count hitting 0
    //                frees the slot for any future block.
    // There is no `base` to advance and nothing to initialise on reopen:
    // an all-zero ring simply means "nothing pending", which is correct
    // (recovery replay runs synchronously before flushers start).
    //
    // earliest_pending(): one relaxed pass over all kWindow slots taking
    // the min tagged block among count>0 slots; empty -> kNoActiveBlock
    // (NO constraint — v1 wrongly returned max_seen+1, which pinned a
    // fresh post-recovery frontier back to ~block 1 and destroyed the
    // O(tail) property). 2^16 sequential u64 loads ≈ 512 KB ≈ tens of µs,
    // and it runs only inside try_write_checkpoint (per-cadence, ~ms), so
    // the scan cost is noise.
    struct PendingBlocks {
        static constexpr std::size_t kWindow = 1u << 16;  // 65536 slots
        static constexpr std::size_t kMask = kWindow - 1;
        static constexpr std::uint64_t kCountMask = 0xFFFF'FFFFull;
        static std::uint64_t pack(std::uint64_t block, std::uint64_t count) {
            return (block << 32) | count;
        }
        static std::uint64_t block_of(std::uint64_t s) { return s >> 32; }
        static std::uint64_t count_of(std::uint64_t s) { return s & kCountMask; }
        std::array<std::atomic<std::uint64_t>, kWindow> slot{};
    };
    PendingBlocks pending_{};

    // Backpressure for the bounded pending ring (Claim 5A). When
    // pending_register hits a slot still tagged by an OLDER block (a residue
    // collision — the flusher is >= kWindow blocks behind), the producer must
    // WAIT for that slot to drain rather than abort: the ring is a bounded
    // producer-consumer window, and a full window means "slow down", not "bug".
    // The waiter blocks on pending_cv_ (a DEDICATED mutex, never flusher_mus_ —
    // pending_retire runs under flusher_mus_[id] inside apply_batch, so reusing
    // it would deadlock); pending_retire notifies after a slot count reaches 0.
    // pending_retire_gen_ is bumped on every slot-freeing retire so a waiter
    // that missed the notify still makes progress (wait_for on the generation).
    std::mutex pending_cv_mu_;
    std::condition_variable pending_cv_;
    std::atomic<std::uint64_t> pending_retire_gen_{0};
    // Fail-fast backstop: if a producer waits this long, a flusher is genuinely
    // dead/stuck (not merely behind), so abort with full diagnostics rather than
    // hang forever. Generous — real backpressure clears in ms.
    static constexpr std::uint64_t kPendingStallTimeoutNs = 60ull * 1000000000ull;

    // Monotonic nanosecond clock for backpressure wait accounting.
    static std::uint64_t now_ns() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Wake flushers so a backpressured producer can make progress. Thin wrapper
    // over wake_all_flushers() named for intent at the pending_register call
    // site (declared before wake_all_flushers, defined out of line below is
    // unnecessary — wake_all_flushers is a member already in scope).
    void hiom_wake_flushers_for_backpressure() { wake_all_flushers(); }

    void pending_register(std::uint64_t b) {
        // block:32 packing headroom: 2^32 blocks × 24 KiB = 96 TiB of
        // VPages — far beyond any pool this code addresses. Guarded so a
        // corrupt offset fails loudly instead of aliasing the tag.
        if (b > PendingBlocks::kCountMask) {
            std::fprintf(stderr,
                "[HiOM FATAL] pending_register: block %llu exceeds 32-bit "
                "tag range — corrupt offset?\n",
                (unsigned long long)b);
            std::abort();
        }
        auto& s = pending_.slot[b & PendingBlocks::kMask];
        std::uint64_t cur = s.load(std::memory_order_acquire);
        std::uint64_t stall_start_ns = 0;  // 0 until we first stall
        bool counted_stall = false;
        for (;;) {
            std::uint64_t desired;
            if (PendingBlocks::count_of(cur) == 0) {
                desired = PendingBlocks::pack(b, 1);          // claim free slot
            } else if (PendingBlocks::block_of(cur) == b) {
                if (PendingBlocks::count_of(cur) == PendingBlocks::kCountMask) {
                    // cur+1 would carry into the block tag. 2^32 pending
                    // entries for one block cannot happen legitimately
                    // (a block holds far fewer records) — fail fast like
                    // every other accounting violation here.
                    std::fprintf(stderr,
                        "[HiOM FATAL] pending_register(%llu): count "
                        "saturated — register/retire imbalance?\n",
                        (unsigned long long)b);
                    std::abort();
                }
                desired = cur + 1;                            // same block: +1
            } else {
                // Residue collision: slot still tagged by an OLDER block b'≠b
                // whose entries have not yet been retired (flusher >= kWindow
                // blocks behind). This is NOT an accounting bug — the ring is a
                // bounded window and it is momentarily full for this residue.
                // Back off: wake the flushers and WAIT for slot b' to drain,
                // then retry the CAS. The colliding older block was registered
                // (and therefore enqueued) strictly before this call, so a
                // flusher can drain it independently of this producer — no
                // cycle. See the register-before-enqueue ordering in
                // push_commit and the dedicated-mutex note on pending_cv_mu_.
                if (!counted_stall) {
                    stats_.pending_ring_stalls.fetch_add(
                        1, std::memory_order_relaxed);
                    counted_stall = true;
                    stall_start_ns = now_ns();
                }
                hiom_wake_flushers_for_backpressure();
                {
                    std::unique_lock<std::mutex> lk(pending_cv_mu_);
                    const std::uint64_t gen_before
                        = pending_retire_gen_.load(std::memory_order_acquire);
                    // Re-check under the lock: the retire may have already
                    // fired (and bumped the gen) between our CAS-fail and here.
                    if (s.load(std::memory_order_acquire) == cur
                        && pending_retire_gen_.load(std::memory_order_acquire)
                               == gen_before) {
                        pending_cv_.wait_for(
                            lk, std::chrono::milliseconds(50),
                            [&] {
                                return pending_retire_gen_.load(
                                           std::memory_order_acquire)
                                       != gen_before;
                            });
                    }
                }
                const std::uint64_t waited_ns = now_ns() - stall_start_ns;
                if (waited_ns > kPendingStallTimeoutNs) {
                    // A flusher is genuinely stuck/dead, not merely behind:
                    // fail fast with full diagnostics rather than hang forever.
                    std::fprintf(stderr,
                        "[HiOM FATAL] pending-ring backpressure TIMEOUT after "
                        "%.1fs: new block %llu vs stuck block %llu at ring slot "
                        "%llu (window %zu), commit-buffer depth ~%zu, flushers "
                        "%zu — flusher stalled/dead, not just behind\n",
                        waited_ns / 1e9,
                        (unsigned long long)b,
                        (unsigned long long)PendingBlocks::block_of(cur),
                        (unsigned long long)(b & PendingBlocks::kMask),
                        PendingBlocks::kWindow,
                        commit_buf_ ? commit_buf_->size_hint() : 0,
                        fcfg_.num_flushers);
                    std::abort();
                }
                cur = s.load(std::memory_order_acquire);      // retry classify
                continue;
            }
            if (s.compare_exchange_weak(cur, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
                if (counted_stall) {
                    stats_.pending_ring_wait_ns.fetch_add(
                        now_ns() - stall_start_ns, std::memory_order_relaxed);
                }
                return;
            }
            // cur reloaded on failure; re-classify.
        }
    }

    void pending_retire(std::uint64_t b) {
        auto& s = pending_.slot[b & PendingBlocks::kMask];
        std::uint64_t cur = s.load(std::memory_order_acquire);
        for (;;) {
            if (PendingBlocks::block_of(cur) != b
                    || PendingBlocks::count_of(cur) == 0) {
                // Register/retire are strictly paired (kPut-with-offset
                // only, both sides use the same guard), so this is an
                // accounting bug, not a recoverable state.
                std::fprintf(stderr,
                    "[HiOM FATAL] pending_retire(%llu): slot holds "
                    "{block=%llu, count=%llu} — register/retire imbalance\n",
                    (unsigned long long)b,
                    (unsigned long long)PendingBlocks::block_of(cur),
                    (unsigned long long)PendingBlocks::count_of(cur));
                std::abort();
            }
            if (s.compare_exchange_weak(cur, cur - 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
                // If this retire FREED the slot (count hit 0), a producer may be
                // backpressured waiting for exactly this residue. Bump the
                // generation and notify. Done under pending_cv_mu_ so a waiter
                // that is between its slot-recheck and its wait_for cannot miss
                // the wakeup (the mutex serialises the gen bump with the wait).
                // pending_cv_mu_ is NOT flusher_mus_[id] (which this call holds
                // via apply_batch), so there is no lock-order inversion.
                if (PendingBlocks::count_of(cur - 1) == 0) {
                    {
                        std::lock_guard<std::mutex> lk(pending_cv_mu_);
                        pending_retire_gen_.fetch_add(
                            1, std::memory_order_acq_rel);
                    }
                    pending_cv_.notify_all();
                }
                return;
            }
        }
    }

    // Min tagged block over all slots with count>0, or kNoActiveBlock if
    // nothing is pending (= no constraint on the frontier). Exact w.r.t.
    // the interleaving proof in try_write_checkpoint: every load is a
    // whole-slot snapshot, so a block is either seen pending or has been
    // durably retired — never misattributed.
    std::uint64_t earliest_pending_block() const {
        std::uint64_t min_b = kNoActiveBlock;
        for (std::size_t i = 0; i < PendingBlocks::kWindow; ++i) {
            const std::uint64_t s
                = pending_.slot[i].load(std::memory_order_acquire);
            if (PendingBlocks::count_of(s) == 0) continue;
            const std::uint64_t b = PendingBlocks::block_of(s);
            if (b < min_b) min_b = b;
        }
        return min_b;
    }

    // -- Read-path telemetry shards (per-Client, contention-free) ------
    //
    // One shard per Client slot, mirroring client_slots_ above. Each
    // HiOM::Client increments read_shards_[slot_idx_] on its get() hot
    // path via the HIOM_RSTAT_INC macro using PLAIN (non-atomic) adds:
    // the §Concurrency contract is one thread per Client, so each shard
    // has a single writer and needs no atomic. alignas(64) gives every
    // shard its own cache line, so two Clients pinned to two cores never
    // false-share. This replaces the single contended Stats atomic that
    // used to cap read scaling (see the HIOM_RSTAT_INC comment at file
    // top for the measured wall + ablation).
    //
    // Shards are intentionally NOT reset when a Client releases its slot
    // (release_client_slot leaves them intact, exactly like ClientSlot's
    // last_block), so totals survive Client destruction and a later
    // stats() fold still sees them. reserve_client_slot does not clear
    // them either — a reused slot keeps accumulating, which is correct
    // because the per-slot sum over the whole run is all the fold needs.
    struct alignas(64) ReadStatShard {
        std::uint64_t hot_hits{0};
        std::uint64_t hot_misses{0};
        std::uint64_t hot_fp_collisions{0};
        std::uint64_t cold_hits{0};
        std::uint64_t cold_misses{0};
        std::uint64_t cold_fp_collisions{0};
        std::uint64_t hot_warmups{0};
        // Transient read retries: a get() that spun over a concurrent in-place
        // update's VPage lock window rather than failing. Diagnostic only —
        // nonzero is healthy (the retry is doing its job); it does NOT count
        // as a read failure. Kept in the shard so it stays contention-free.
        std::uint64_t cold_read_retries{0};
    };
    static_assert(sizeof(ReadStatShard) == 64,
                  "ReadStatShard should occupy exactly one cache line");
    std::array<ReadStatShard, kMaxClientSlots> read_shards_{};

    // Fold the per-Client read shards into the Stats aggregate's read
    // counters. Called by stats() at telemetry time.
    //
    // Thread-safety: MUST be called at a point quiescent w.r.t. the read
    // path (no Client concurrently inside get()). All current callers
    // satisfy this — the YCSB fixture reads stats() after the timed loop
    // joins all worker threads, and the integration tests read it
    // single-threaded between op batches — so the plain-uint64 loads
    // race with nothing. O(kMaxClientSlots): negligible, off the hot path.
    void fold_read_shards_() {
        std::uint64_t hh = 0, hm = 0, hfc = 0, ch = 0, cm = 0, cfc = 0, hw = 0,
                      crr = 0;
        for (const auto& s : read_shards_) {
            hh += s.hot_hits;            hm  += s.hot_misses;
            hfc += s.hot_fp_collisions;  ch  += s.cold_hits;
            cm += s.cold_misses;         cfc += s.cold_fp_collisions;
            hw += s.hot_warmups;         crr += s.cold_read_retries;
        }
        stats_.hot_hits.store(hh, std::memory_order_relaxed);
        stats_.hot_misses.store(hm, std::memory_order_relaxed);
        stats_.hot_fp_collisions.store(hfc, std::memory_order_relaxed);
        stats_.cold_hits.store(ch, std::memory_order_relaxed);
        stats_.cold_misses.store(cm, std::memory_order_relaxed);
        stats_.cold_fp_collisions.store(cfc, std::memory_order_relaxed);
        stats_.hot_warmups.store(hw, std::memory_order_relaxed);
        stats_.cold_read_retries.store(crr, std::memory_order_relaxed);
    }
    // ------------------------------------------------------------------

    // Reserve a slot. Returns the index, or kInvalidSlotIdx if all slots
    // are taken. When a checkpoint is attached, an untracked client would
    // let the frontier advance past its unflushed writes (silent data loss
    // on crash), so the caller (get_client) fail-fasts instead — see there.
    //
    // Publish-order safety (CAS active=true FIRST, then reset the block
    // fields): a checkpoint that reads the slot in the window between the
    // claim and the resets sees active=true with the PREVIOUS owner's
    // stale fields. Both possible stale values are safe:
    //   - a stale finite block: at most it drags the frontier LOWER than
    //     necessary (over-covers) — conservative, never unsound;
    //   - kNoActiveBlock (no constraint): sound because the NEW owner has
    //     no durable-but-untracked record yet — its first put publishes
    //     inflight_block (t0) before the record can become durable (t1,
    //     see the proof in try_write_checkpoint), so by the time this
    //     client has anything to protect, its fields do constrain.
    // The reverse order (reset fields BEFORE the claim CAS) would be
    // UNSOUND: a losing claimant's straggling kNoActiveBlock store could
    // clobber the winner's already-published real inflight bound.
    std::size_t reserve_client_slot() {
        for (std::size_t i = 0; i < kMaxClientSlots; ++i) {
            bool expected = false;
            if (client_slots_[i].active.compare_exchange_strong(
                    expected, true,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                client_slots_[i].last_block.store(
                    kNoActiveBlock, std::memory_order_release);
                client_slots_[i].inflight_block.store(
                    kNoActiveBlock, std::memory_order_release);
                return i;
            }
        }
        return kInvalidSlotIdx;
    }

    // Release a slot. Clears `active` only. The old code relied on
    // last_block staying intact after release to keep the frontier from
    // advancing past a departed client's still-buffered entries — but that
    // reasoning was WRONG (the real SIGKILL test disproved it): last_block
    // tracks only the client's LATEST block, not the earlier blocks it left
    // with entries still in the commit buffer, so a released (or even still
    // active) client could not, by itself, hold the frontier back correctly.
    // The authoritative guard is now the pending-block tracker (Claim 5A):
    // every buffered entry is registered against its block and only retired
    // after ColdTier durably applies it, so earliest_pending_block() — not
    // last_block — is what pins the frontier. min_active_writer_block is a
    // secondary bound that also covers the pre-register window (5B) for
    // active clients; on release we simply stop counting this slot, which
    // is safe because any entry it already pushed is tracked by pending_.
    void release_client_slot(std::size_t idx) {
        if (idx >= kMaxClientSlots) return;
        client_slots_[idx].active.store(false, std::memory_order_release);
    }

    void note_client_block(std::size_t idx, std::uint64_t block_number) {
        if (idx >= kMaxClientSlots) return;
        client_slots_[idx].last_block.store(block_number,
                                            std::memory_order_release);
    }

    // Publish a conservative in-flight block lower bound BEFORE a put's
    // VPage write (Claim 5B). Monotonic guard: never raise inflight above
    // an already-lower value we might still owe a register for — actually
    // the reverse: we store the CURRENT owned block, which only ever moves
    // forward as the client fills blocks, and is always <= the block the
    // imminent put lands in. The frontier takes the min of this and
    // last_block, so a torn put (durable VPage, not-yet-registered) is
    // still covered.
    void note_inflight_block(std::size_t idx, std::uint64_t block_number) {
        if (idx >= kMaxClientSlots) return;
        client_slots_[idx].inflight_block.store(block_number,
                                                std::memory_order_release);
    }

    // Min over all active slots of min(last_block, inflight_block).
    // last_block is the last completed write; inflight_block is a
    // pre-write lower bound. Together they bound the lowest block any
    // active client may have durable-but-unflushed. Returns kNoActiveBlock
    // if no slot is active. Composed by try_write_checkpoint with
    // earliest_pending_block() and viper_next into the safe frontier.
    std::uint64_t min_active_writer_block() const {
        std::uint64_t m = kNoActiveBlock;
        for (auto& s : client_slots_) {
            if (!s.active.load(std::memory_order_acquire)) continue;
            const auto lb = s.last_block.load(std::memory_order_acquire);
            const auto ib = s.inflight_block.load(std::memory_order_acquire);
            const auto b = std::min(lb, ib);
            if (b < m) m = b;
        }
        return m;
    }
    // ----------------------------------------------------------------

    // M4 Phase C / Step 2 / Step 3: drain every non-empty lane *owned
    // by `flusher_id`* until all observed empty, then sort each lane's
    // batch by (fp64 asc, seq asc), coalesce same-fp runs in
    // apply_batch, and apply. Caller must hold flusher_mus_[flusher_id].
    // Returns total drained.
    //
    // Per-lane apply: same fp64 always lands in the same lane (by
    // CommitBuffer::lane_of_fp64), so a per-lane batch is sufficient
    // for the (fp64, seq) winner picker — there's no fp that spans
    // lanes. The flusher walks the nonempty_mask bits masked to its
    // owned lanes, drains each set lane to (its consumer's view of)
    // empty, applies, and re-checks the mask in case producers re-set
    // bits during apply.
    std::size_t drain_to_empty_and_apply_locked(std::size_t flusher_id) {
        flushing_in_progress_[flusher_id].store(
            true, std::memory_order_release);
        const std::uint64_t my_lanes_mask
            = lanes_mask_for_flusher(flusher_id, fcfg_.num_flushers);
        std::vector<CommitEntry> batch;
        std::size_t total = 0;
        constexpr std::size_t kPerCall = 1024;
        while (true) {
            const std::uint64_t mask
                = commit_buf_->nonempty_mask() & my_lanes_mask;
            if (mask == 0) break;
            std::uint64_t bits = mask;
            std::size_t round_total = 0;
            while (bits) {
                const std::size_t lane
                    = static_cast<std::size_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                std::size_t lane_total = 0;
                while (true) {
                    const std::size_t got = commit_buf_->try_drain_lane(
                        *flusher_consumer_toks_[lane], lane, batch, kPerCall);
                    if (got == 0) break;
                    lane_total += got;
                }
                if (lane_total > 0) {
                    std::stable_sort(
                        batch.begin(), batch.end(),
                        [](const CommitEntry& a, const CommitEntry& b) {
                            if (a.fp64 != b.fp64) return a.fp64 < b.fp64;
                            return a.seq < b.seq;
                        });
                    apply_batch(batch);
                    stats_.commits_flushed.fetch_add(
                        lane_total, std::memory_order_relaxed);
                    round_total += lane_total;
                    batch.clear();
                }
            }
            total += round_total;
            // mask bit was set but lane was already drained by another
            // pass through this loop (rare, comes from the nonempty_mask
            // race window): nothing more to do this round.
            if (round_total == 0) break;
        }
        flushing_in_progress_[flusher_id].store(
            false, std::memory_order_release);
        return total;
    }

    // Apply a sorted batch of CommitEntry to the cold tier and the
    // HotTier state machine. Extracted from drain_once so the inline-
    // flush path (M4 Phase B) can reuse it. Caller must already hold
    // a (logical or physical) drain ownership — the function makes no
    // assumption about which thread is calling, but Concurrency-wise
    // the CAS dance on each slot is safe regardless.
    // Apply a sorted batch of CommitEntry to the cold tier and the
    // HotTier state machine. M4 Phase C: batch is sorted by
    // (fp64 asc, seq asc). For each run of equal fp64, we apply
    // ONLY one cold-tier op per fp (idempotent + bandwidth saving),
    // but we still walk every entry's hot_slot to drive the
    // PINNED → IN_FLUSH → UNPINNED transitions. This resolves the
    // multi-producer "two threads write same key" race where without
    // a (fp64, seq) sort, ColdTier could end up with the older offset.
    //
    // Winner selection (Step 5c): walk descending from highest seq.
    // The first kRemove or alive kPut wins. A kPut entry is "alive"
    // for our purposes only if (a) the slot's free_slots bit is
    // unset (slot is occupied) AND (b) the slot's stored key hashes
    // to the same fp64 as the entry. Just (a) is insufficient
    // because under heavy update concurrency a freed slot can be
    // *reused* by a different key — alive=true, but the data is
    // not for us. Hashing the slot's key catches that. If the
    // entire run is stale kPuts (or reused-by-other-key kPuts),
    // we leave ColdTier untouched: any earlier-batch upsert for the
    // same fp stays in place, and the future batch carrying the
    // latest seq will overwrite with the correct offset. (Falling
    // back to kRemove here would wipe a still-valid earlier entry.)
    void apply_batch(std::vector<CommitEntry>& batch) {
        if (batch.empty()) return;

        // Two-stage apply (M3 follow-up: cache-line-aligned bulk
        // writes to ColdTier).
        //
        // Stage 1: walk the (fp64,seq)-sorted batch run-by-run,
        //   pick a winner per run, classify it as kPut or kRemove,
        //   and either accumulate kPut winners into `puts` (later
        //   dispatched via cold_->bulk_upsert) or apply kRemove
        //   immediately (rare; kRemove targets an existing fp,
        //   so the bucket walk is per-key by nature).
        //
        // Stage 2: drive the HotTier state machine
        //   (PINNED → IN_FLUSH → UNPINNED) for every entry. This
        //   step happens AFTER the cold-tier writes are durable,
        //   preserving M4 Phase B's invariant that the slot stays
        //   pinned across the whole run until ColdTier owns the
        //   record.
        std::vector<typename ColdTier::BulkEntry> puts;
        puts.reserve(batch.size());

        std::size_t i = 0;
        while (i < batch.size()) {
            std::size_t j = i + 1;
            while (j < batch.size() && batch[j].fp64 == batch[i].fp64) {
                ++j;
            }
            // batch[i..j) is one fp64 run.
            //
            // Winner-picker (M3 follow-up #2 / 2026-05-09): HotTier
            // truth as the primary path, alive-and-fp-match descending
            // walk as the fallback.
            //
            // Why HotTier truth: P0 retired CCEH from the write path,
            // making HotTier::upsert_pinned the linearization point for
            // same-fp32 writes. The slot's current (fp32, packed_off)
            // identifies the latest CAS-winner, which is the canonical
            // offset for this key. Picking the batch entry whose off
            // matches that snapshot reproduces the canonical winner
            // without depending on a global seq's after-CAS ordering.
            // (Historical context: the original M4 Phase C design used
            // a global commit_seq_ atomic bumped after the Viper write.
            // A CAS winner could be preempted between CAS and the
            // fetch_add and end up with a smaller seq than a later
            // CAS-loser, producing a flaky picker. HotTier-truth removes
            // that dependency. The global commit_seq_ was retired in
            // M3 follow-up #2 Step 1 — seq is now per-Client local,
            // load-bearing only for the rare fallback walk below.)
            //
            // Fallback covers: HotTier slot evicted out of the PINNED
            // window, fp32 collision (slot now belongs to a different
            // key), and HotTier holds a future-batch off (canonical
            // entry not in this batch). The descending walk's existing
            // alive-and-fp-match logic handles all three correctly,
            // and seq still acts as the tiebreaker among multiple alive
            // kPuts (rare, e.g., concurrent same-key writes from
            // different threads where neither's free_occupied_slot has
            // landed yet).
            const CommitEntry* winner = nullptr;

            // Step 1: pick any kPut entry's hot_slot in the run. All
            // kPuts targeting the same fp32 share the same HotTier slot
            // (upsert_pinned overwrites in place via CAS), so any
            // valid hot_slot in the run points at the same atomic.
            HotTier::SlotRef ref{};
            for (std::size_t k = i; k < j; ++k) {
                if (batch[k].op == CommitEntry::Op::kPut
                    && batch[k].hot_slot.valid) {
                    ref = batch[k].hot_slot;
                    break;
                }
            }

            // Step 2: HotTier-truth fast path.
            if (ref.valid) {
                const auto view = hot_.read_slot(ref);
                if (view.fp != HotTier::kEmptyFp) {
                    const auto d = decode(view.packed_off,
                                          route_to_region_default(),
                                          base_map_);
                    const KVOffset canonical_off{
                        static_cast<viper::block_size_t>(d.block_number),
                        static_cast<viper::page_size_t>(d.page_number),
                        static_cast<viper::data_offset_size_t>(
                            d.data_offset)};
                    for (std::size_t k = i; k < j; ++k) {
                        if (batch[k].op != CommitEntry::Op::kPut) continue;
                        if (batch[k].off != canonical_off) continue;
                        // fp64 verify catches the (rare) case where the
                        // batch entry's slot was freed and reused by
                        // another key whose fp32 happens to also collide
                        // — without the verify, ColdTier could end up
                        // pointing at the wrong key's data on PM.
                        K stored_key;
                        if (!viper_.hiom_get_slot_key(batch[k].off,
                                                      &stored_key)) {
                            continue;
                        }
                        if (key_fingerprint64(stored_key) != batch[k].fp64) {
                            continue;
                        }
                        winner = &batch[k];
                        break;
                    }
                }
            }

            // Step 3: fallback descending walk (alive-and-fp-match,
            // seq tiebreaker). Triggered when (a) HotTier slot was
            // cleared (kRemove won, or eviction post-PINNED), (b) fp32
            // collision overwrote the slot, or (c) HotTier holds an
            // off from a future batch.
            if (winner == nullptr) {
                for (std::size_t k = j; k > i; --k) {
                    const CommitEntry& e = batch[k - 1];
                    if (e.op == CommitEntry::Op::kRemove) {
                        winner = &e;
                        break;
                    }
                    K stored_key;
                    if (!viper_.hiom_get_slot_key(e.off, &stored_key)) {
                        continue;  // freed
                    }
                    if (key_fingerprint64(stored_key) != e.fp64) {
                        continue;  // slot reused by a different key
                    }
                    winner = &e;
                    break;
                }
            }

            if (winner != nullptr) {
                if (winner->op == CommitEntry::Op::kPut) {
                    puts.push_back({winner->fp64, winner->off});
                } else {
                    // kRemove: dispatch immediately. Single-key path
                    // is fine — removes are rare relative to puts and
                    // each one targets an existing chain entry, not
                    // a clean slot, so a "bulk_remove" wouldn't
                    // amortise much (no shared-bucket-header flip).
                    cold_->remove(winner->fp64);
                }
            }
            i = j;
        }

        // Stage 1 dispatch: amortise the PM fence across all kPut
        // winners by sorting them once into bucket-runs and
        // emitting two drains per run (instead of two per entry).
        if (!puts.empty()) {
            const std::size_t wrote = cold_->bulk_upsert(puts);
            if (wrote != puts.size()) {
                // ColdTier overflow-pool exhausted: bulk_upsert stopped
                // early and index entries were silently dropped. Under a
                // benchmark this must fail hard, not corrupt results —
                // size the tier via ColdTier::sizing_for(prefill).
                std::fprintf(stderr,
                    "[HiOM FATAL] ColdTier overflow in apply_batch: wrote "
                    "%zu/%zu entries — index sizing too small\n",
                    wrote, puts.size());
                std::abort();
            }
        }

        // Durable-frontier retire (Claim 5C): every entry in this batch is
        // now durable in ColdTier — winners via bulk_upsert above (or
        // cold_->remove for kRemove), and coalesced losers will NEVER get
        // their own ColdTier write (the winner's offset supersedes them),
        // so both are safe to retire. Retiring only winners would leak the
        // losers' pending counts forever, pinning the frontier. Runs after
        // the ColdTier drains and BEFORE the checkpoint hook below, so the
        // frontier this batch may publish already reflects these retires.
        if (checkpoint_ != nullptr) {
            for (std::size_t k = 0; k < batch.size(); ++k) {
                // Symmetric with push_commit's register guard: only kPut
                // with a real offset was registered.
                if (batch[k].op == CommitEntry::Op::kPut
                        && !batch[k].off.is_tombstone()) {
                    pending_retire(
                        static_cast<std::uint64_t>(batch[k].off.block_number));
                }
            }
        }

        // Stage 2: HotTier state-machine drives. The CAS dance is
        // unchanged from M4 Phase C — we walk every entry's slot
        // ref. CAS failures are benign:
        //  - earlier-seq entries' slot refs may point at slots
        //    whose state was already advanced by the latest
        //    upsert_pinned for the same fp (last writer wins on
        //    the underlying slot);
        //  - or the slot may already have transitioned through
        //    IN_FLUSH→UNPINNED by a prior apply pass.
        for (std::size_t k = 0; k < batch.size(); ++k) {
            const bool we_own = hot_.cas_slot_state(
                batch[k].hot_slot,
                HotTier::SlotState::kPinned,
                HotTier::SlotState::kInFlush);
            if (we_own) {
                hot_.cas_slot_state(
                    batch[k].hot_slot,
                    HotTier::SlotState::kInFlush,
                    HotTier::SlotState::kUnpinned);
            }
        }


        // M5 checkpoint hook: bump cumulative flushed count and, for
        // each cadence boundary the batch crossed, try to persist a
        // fresh checkpoint. Pre–M3-bulk-upsert, batches were small
        // enough that one boundary per batch was the common case and
        // a single try_write_checkpoint() per apply_batch was
        // sufficient. After bulk_upsert lands, the flusher coalesces
        // many entries per batch and one apply_batch can span 2+
        // cadence_entries boundaries — emit a checkpoint per crossed
        // boundary to keep the user-visible cadence honest. (Each
        // try_write_checkpoint is try_lock–guarded and idempotent on
        // failure, so a small loop is safe.)
        if (checkpoint_ != nullptr && !batch.empty()) {
            const auto before = flushed_count_.fetch_add(
                batch.size(), std::memory_order_acq_rel);
            const auto after = before + batch.size();
            const std::uint64_t cadence = ccfg_.cadence_entries;
            if (cadence > 0) {
                const auto boundaries_before = before / cadence;
                const auto boundaries_after = after / cadence;
                for (auto b = boundaries_before; b < boundaries_after; ++b) {
                    try_write_checkpoint();
                }
            }
        } else if (checkpoint_ == nullptr && !batch.empty()) {
            // Even without a checkpoint attached, keep the cumulative
            // count moving so HiOM::flushed_count() reads truthfully.
            flushed_count_.fetch_add(batch.size(),
                                     std::memory_order_acq_rel);
        }
    }

    // M5: write a fresh checkpoint into the inactive A/B slot. Multiple
    // threads (background flusher + inline-flusher) can both call
    // apply_batch concurrently and race here; try_lock keeps it to one
    // writer at a time and the loser silently skips this round (the
    // next batch's cadence check picks it up). Inside the lock, ++seq_
    // is the linearization point — guarantees strict monotonicity in
    // the persisted record sequence.
    void try_write_checkpoint() {
        std::unique_lock<std::mutex> lk(checkpoint_mu_, std::try_to_lock);
        if (!lk.owns_lock()) return;
        CheckpointRecord rec{};
        rec.seq = seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
        rec.flushed_count = flushed_count_.load(std::memory_order_acquire);
        // Claim 5: vpage_frontier is a DURABLE frontier — every live record
        // in a block below it is guaranteed already persisted in ColdTier.
        // It is the min of three bounds:
        //   (a) viper_next — Viper's next-to-claim block (nothing above
        //       exists yet);
        //   (b) min_active_writer_block — per active client, min(last_block,
        //       inflight_block); covers the pre-register window (5B) where a
        //       durable VPage write hasn't reached the commit buffer yet;
        //   (c) earliest_pending_block — the earliest block with a commit-
        //       buffer entry not yet durably applied to ColdTier (5A). This
        //       is the load-bearing bound the old min(writer,viper) LACKED:
        //       a writer can be far past a block whose entries are still
        //       buffered, and only (c) holds the frontier back to it.
        // The old code used only min(a,b) — writer positions, not flush
        // progress — which the real-SIGKILL test proved loses records.
        //
        // ---- Linearization proof (why these three racy snapshots compose
        // ---- into a sound frontier; x86-TSO + the fences already present):
        //
        // Per put, in the writing client's program order:
        //   t0: inflight_block := B0 (release store), B0 <= B where B is
        //       the block the record lands in (the client writes its owned
        //       block or claims a fresh HIGHER one — this monotonicity is a
        //       PRECONDITION, enforced by the constructor's reclamation
        //       fail-fast: with block reuse, get_new_block can hand out a
        //       reclaimed LOW block and B0 <= B is false);
        //   t1: viper_.put persists the record — its pmem_persist issues
        //       SFENCE, which drains the store buffer, so the t0 store is
        //       GLOBALLY VISIBLE no later than the record is durable;
        //   t2: pending_register(B) — a LOCKED RMW, globally visible
        //       immediately, and TSO keeps it ordered before any later
        //       store of this thread;
        //   t0': the NEXT put's inflight store (t2 < t0' by program order).
        // So the record's coverage is gapless: inflight covers [t0, t0'),
        // pending covers [t2, retire), and t2 < t0' — from before the
        // record can be durable until its ColdTier apply is durable (only
        // apply_batch retires, strictly after the bulk_upsert drain), at
        // least one bound covers block B.
        //
        // The checkpoint must therefore observe at least one cover. It
        // reads the client slots FIRST and the pending ring LAST (loads
        // are not reordered on TSO). Case split on its slot read at Ts:
        //   - Ts sees inflight <= B: bound (b) covers B. Done.
        //   - Ts misses it (reads a later inflight > B): then Ts >= t0'
        //     in visibility order, and t2 < t0' <= Ts < Tr (the ring
        //     read), so Tr observes the registered count — unless it was
        //     already retired, which by the retire ordering means the
        //     record is durable in ColdTier and needs no cover. Done.
        // Reading the ring BEFORE the slots would break this case split —
        // do not reorder (a)/(b) below after (c).
        //
        // Portability caveat: the argument leans on TSO (total store
        // order + no load-load reordering) and on put's internal SFENCE.
        // This codebase is x86-only (clwb); a port to a weaker memory
        // model must add an explicit fence between t0 and t1 and make the
        // checkpoint's three reads a seq_cst sequence.
        const std::uint64_t client_min = min_active_writer_block();
        const std::uint64_t viper_next
            = KVOffset{viper_.hiom_vpage_frontier()}.block_number;
        const std::uint64_t pending_min = earliest_pending_block();
        const std::uint64_t safe_block
            = std::min(std::min(client_min, viper_next), pending_min);
        rec.vpage_frontier = KVOffset{
            static_cast<viper::block_size_t>(safe_block), 0, 0
        }.offset;
        rec.cold_size = (cold_ != nullptr) ? cold_->approx_size() : 0;
        // 5D: stamp the durable-frontier protocol version so a future open
        // knows this frontier is trustworthy (old v0 pools are not).
        rec.reserved[0] = CheckpointRecord::kProtoDurableFrontier;
        checkpoint_->write(rec);
        stats_.checkpoints_written.fetch_add(1, std::memory_order_relaxed);
    }

    // M6: bounded VPage tail scan. Walks Viper VPages in
    // [frontier_block, current_block) — derived from
    // checkpoint.vpage_frontier (or 0 if no valid checkpoint) and
    // viper.hiom_vpage_frontier() — and upserts every live record into
    // ColdTier. Idempotent: ColdTier::upsert is last-write-wins on the
    // (fp64, offset) pair, and Viper's update path is in-place so each
    // key has at most one live offset on PM. Parallelised by chunking
    // the block range across `threads` workers in the same shape as
    // Viper::recover_database.
    //
    // Returns the count of records replayed, for stats and test
    // introspection. Caller increments stats_.recovery_replayed.
    std::size_t recover_tail_into_cold(std::size_t threads) {
        if (cold_ == nullptr) return 0;
        viper::block_size_t frontier_block = 0;
        if (checkpoint_ != nullptr) {
            if (auto rec = checkpoint_->read_valid()) {
                // 5D: only trust the stored frontier if it was written by
                // the durable-frontier protocol (v2). Older pools (v0)
                // recorded min(writer positions), which is NOT a durable
                // frontier — a record below it may never have reached
                // ColdTier. For those, recover conservatively from block 0
                // (frontier_block stays 0) so nothing is missed.
                if (rec->proto_version()
                        == CheckpointRecord::kProtoDurableFrontier) {
                    frontier_block
                        = KVOffset{rec->vpage_frontier}.block_number;
                }
            }
        }
        // current_block_page_.block_number semantics: the *next* block
        // a client will claim (get_new_block bumps it from
        // {client_block, ...} to {client_block + 1, ...}). So when
        // checkpoint fires while a client is writing into block N,
        // frontier_block is captured as N+1 — the boundary block N
        // itself is NOT covered by [frontier_block, current_block).
        // That block typically straddles drained-and-undrained
        // entries (first-half drained pre-checkpoint, second-half
        // committed-but-not-drained post-checkpoint). Start one block
        // earlier so the boundary is included; ColdTier::upsert is
        // idempotent on (fp64, same offset), so re-upserting already-
        // in-cold entries is harmless and doesn't double-count
        // num_entries.
        const viper::block_size_t start_block
            = (frontier_block > 0) ? frontier_block - 1 : 0;
        const viper::block_size_t current_block
            = KVOffset{viper_.hiom_vpage_frontier()}.block_number;
        if (current_block <= start_block) return 0;

        // (Stale VPage write-locks are cleared by the ctor's O(P)
        // hiom_clear_all_stale_page_locks() BEFORE this replay — a locked
        // page can sit BELOW start_block after an in-place update, so the
        // clear must cover all used pages, not this [start_block,
        // current_block) tail. See the ctor's Step 1.)

        const std::size_t num_blocks
            = static_cast<std::size_t>(current_block - start_block);
        const std::size_t num_threads
            = std::min<std::size_t>(num_blocks,
                                    std::max<std::size_t>(1, threads));
        const std::size_t per_thread
            = (num_blocks + num_threads - 1) / num_threads;

        std::atomic<std::size_t> total{0};
        std::vector<std::thread> workers;
        workers.reserve(num_threads);
        for (std::size_t t = 0; t < num_threads; ++t) {
            const viper::block_size_t lo = static_cast<viper::block_size_t>(
                start_block + t * per_thread);
            const viper::block_size_t hi = static_cast<viper::block_size_t>(
                std::min<std::size_t>(lo + per_thread, current_block));
            if (lo >= hi) continue;
            workers.emplace_back([this, lo, hi, &total]() {
                std::size_t local = 0;
                viper_.hiom_visit_records(lo, hi,
                    [this, &local](const K& key, const V& /*value*/,
                                   KVOffset off) {
                        if (!cold_->upsert(key_fingerprint64(key), off)) {
                            std::fprintf(stderr,
                                "[HiOM FATAL] ColdTier overflow during "
                                "recovery replay — index sizing too small\n");
                            std::abort();
                        }
                        ++local;
                    });
                total.fetch_add(local, std::memory_order_relaxed);
            });
        }
        for (auto& w : workers) w.join();
        return total.load(std::memory_order_relaxed);
    }

    // Single-cycle drain helper, parameterised by flusher_id (Step 3).
    // Each flusher owns a disjoint subset of lanes, so the (blocking)
    // mutex acquired here serializes only that flusher's background
    // drain with its inline-flush siblings. Returns the count drained.
    std::size_t drain_once(std::size_t flusher_id) {
        if (!commit_buf_ || cold_ == nullptr) return 0;
        std::lock_guard<std::mutex> lk(flusher_mus_[flusher_id]);
        return drain_to_empty_and_apply_locked(flusher_id);
    }

    // Background flusher (one per flusher_id under fcfg_.num_flushers):
    // wakes on timer or high-watermark notify and drains its owned
    // lanes in batches.
    //
    // Step 3: each flusher has its own (mu, cv) pair via wake_slots_.
    // A single shared mu (the original flush_mu_) would have
    // serialized every flusher through the wait/release dance,
    // defeating the parallelism. Per-flusher slots let all flushers
    // wait, wake, and drain genuinely independently.
    void flusher_loop(std::size_t flusher_id) {
        const std::uint64_t my_lanes_mask
            = lanes_mask_for_flusher(flusher_id, fcfg_.num_flushers);
        WakeSlot& slot = wake_slots_[flusher_id];
        std::unique_lock<std::mutex> lk(slot.mu);
        while (!stop_.load(std::memory_order_acquire)) {
            stats_.debug_flusher_iters.fetch_add(1, std::memory_order_relaxed);
            // Wake whenever this flusher's owned lanes have any pending
            // entries. high_watermark is a producer-side knob (gates
            // push_commit's wake_all_flushers call); it must NOT also
            // gate the wake-acceptance side, or back-pressure callers
            // notifying us with size_hint() < high_watermark would
            // bounce off this predicate and we'd only make progress
            // on the 5 ms timer — turning each back-pressured push
            // into a multi-millisecond stall. Saw this stuck on
            // KeyType16+ValueType200 prefill where one fp32 bucket's
            // PINNED slots happened to all hash to a peer flusher's
            // lanes; without the fix, 1M prefill didn't finish in
            // 14 minutes.
            slot.cv.wait_for(lk, fcfg_.interval, [this, my_lanes_mask] {
                if (stop_.load(std::memory_order_acquire) || !commit_buf_) {
                    return stop_.load(std::memory_order_acquire);
                }
                return (commit_buf_->nonempty_mask() & my_lanes_mask) != 0;
            });
            lk.unlock();
            std::size_t drained;
            do {
                stats_.debug_drain_calls.fetch_add(1, std::memory_order_relaxed);
                drained = drain_once(flusher_id);
                if (drained == 0) {
                    stats_.debug_drain_returned_zero.fetch_add(
                        1, std::memory_order_relaxed);
                }
            } while (drained > 0
                     && commit_buf_->size_hint() >= fcfg_.high_watermark);
            lk.lock();
        }
    }

    // Wake every flusher (used by stop, watermark notify, flush_and_wait
    // probes). Cheap — just N notify_one calls; predicates filter for
    // each flusher individually.
    void wake_all_flushers() {
        for (std::size_t i = 0; i < fcfg_.num_flushers; ++i) {
            std::lock_guard<std::mutex> lk(wake_slots_[i].mu);
            wake_slots_[i].cv.notify_one();
        }
    }

    // Step 3 helpers used by destructor / shutdown paths.
    bool any_flusher_joinable() const {
        for (auto& t : flushers_) {
            if (t.joinable()) return true;
        }
        return false;
    }
    bool any_flushing() const {
        for (std::size_t i = 0; i < fcfg_.num_flushers; ++i) {
            if (flushing_in_progress_[i].load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    ViperT& viper_;
    HotTier hot_;
    ColdTier* cold_;
    BlockBaseMap base_map_;
    Stats stats_;
    // Set once by the ctor's Step 2 tail-scan timing; read-only thereafter.
    double recovery_tail_scan_ms_{0.0};
    // Set once by the ctor's Step 1 O(P) stale-lock scan; read-only thereafter.
    double recovery_lock_scan_ms_{0.0};
    FlusherConfig fcfg_;
    // Per-lane flusher-wake watermark = high_watermark / kNumLanes.
    // push_commit wakes on the RISING EDGE through this depth
    // (lane_depth == this), see push_commit for the rationale. Init
    // order: must stay after fcfg_, before checkpoint_.
    std::size_t per_lane_high_watermark_;

    std::unique_ptr<CommitBuffer> commit_buf_;
    std::array<std::unique_ptr<moodycamel::ConsumerToken>,
               CommitBuffer::kNumLanes> flusher_consumer_toks_;
    // Step 3: one std::thread per background flusher. Up to
    // CommitBuffer::kNumLanes flushers; the actual count is
    // fcfg_.num_flushers, others stay default-constructed (joinable
    // checks skip them).
    std::array<std::thread, CommitBuffer::kNumLanes> flushers_;
    std::atomic<bool> stop_{false};
    // each flusher sets its own before drain and clears after apply.
    std::array<std::atomic<bool>,
               CommitBuffer::kNumLanes> flushing_in_progress_{};

    // Step 3: per-flusher wake slot (mu + cv). A single shared
    // (mu, cv) would have serialized every flusher through wait_for's
    // implicit lock acquire/release, eating the parallelism we paid
    // for. Each flusher waits on its own slot; wake_all_flushers()
    // walks them.
    struct alignas(64) WakeSlot {
        std::mutex mu;
        std::condition_variable cv;
    };
    std::array<WakeSlot, CommitBuffer::kNumLanes> wake_slots_;

    // Step 3: per-flusher drain mutex. Replaces the single apply_mu_:
    // each flusher serializes its background drain with any
    // writer-side inline-flush that targeted the same flusher's
    // lanes. Because lanes are partitioned (same fp64 → same lane →
    // same flusher), different flushers never share a HotTier slot
    // or ColdTier region, so no cross-flusher mutex is needed.
    std::array<std::mutex, CommitBuffer::kNumLanes> flusher_mus_;

    // M5 checkpoint state. checkpoint_ is non-owning (caller manages
    // the PM file's lifetime so the same Checkpoint can survive HiOM
    // close+reopen for the recovery test). flushed_count_ is the
    // cumulative running total since DB creation, primed from the
    // restored record on construction. seq_ is the writer's local
    // monotonic seq, also primed. checkpoint_mu_ serializes write()
    // calls so the A/B slot flip is single-writer (Checkpoint is
    // single-writer by contract per the comment in checkpoint.hpp).
    Checkpoint* checkpoint_;
    CheckpointConfig ccfg_;
    std::atomic<std::uint64_t> flushed_count_{0};
    std::atomic<std::uint64_t> seq_{0};
    std::mutex checkpoint_mu_;
};

}  // namespace viper::hiom
