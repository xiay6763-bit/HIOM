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
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "viper/cceh.hpp"
#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/commit_buffer.hpp"
#include "viper/hiom/hot_tier.hpp"
#include "viper/hiom/offset_codec.hpp"

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
        std::atomic<std::uint64_t> commits_flushed{0};
        std::atomic<std::uint64_t> checkpoints_written{0};
        std::atomic<std::uint64_t> recovery_replayed{0};
    };

    // Tunables for the background flusher. Defaults are chosen to
    // balance latency (drain in <10 ms steady state) against per-batch
    // overhead. Caller can override at construction.
    struct FlusherConfig {
        std::chrono::milliseconds interval{std::chrono::milliseconds(5)};
        std::size_t high_watermark{1024};   // wake on size_hint() >= this
        std::size_t batch_max{8192};        // drain at most this many per cycle
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
          checkpoint_(checkpoint),
          ccfg_(ccfg)
    {
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
            const std::uint64_t replayed
                = recover_tail_into_cold(rcfg.recovery_threads);
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
                    const std::uint64_t fp64 = key_fingerprint64(key);
                    auto cold_off = cold_->lookup(fp64);
                    if (!cold_off) return KVOffset::Tombstone();
                    const KVOffset off = *cold_off;
                    // Verify: fp64 collision rate is ~1 in 2^64 but
                    // ColdTier upserts are coalesced by fp so the
                    // same fp can briefly point at a stale offset
                    // during tombstone GC. Cheap key-verify keeps
                    // semantics identical to map_.Get(key, key_check_fn).
                    K stored_key{};
                    V stored_val{};
                    auto ro = viper_.get_read_only_client();
                    if (!ro.hiom_read_at_offset(off, &stored_key,
                                                &stored_val)) {
                        return KVOffset::Tombstone();  // page locked / torn
                    }
                    if (!(stored_key == key)) {
                        return KVOffset::Tombstone();  // fp collision
                    }
                    return off;
                });

            commit_buf_ = std::make_unique<CommitBuffer>();
            flusher_consumer_tok_ = std::make_unique<moodycamel::ConsumerToken>(
                commit_buf_->make_consumer_token());
            flusher_ = std::thread([this]() { flusher_loop(); });
        }
    }

    ~HiOM() {
        if (flusher_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(flush_mu_);
                stop_ = true;
            }
            flush_cv_.notify_all();
            flusher_.join();
            // Drain anything left synchronously, just in case.
            drain_once(/*max=*/std::numeric_limits<std::size_t>::max());
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
              prod_tok_(std::move(other.prod_tok_)),
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

        bool put(const K& key, const V& value) {
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
            const bool is_new_item = viper_.put(key, value);

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
                    hiom_.stats_.hot_hits.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                hiom_.stats_.hot_fp_collisions.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                hiom_.stats_.hot_misses.fetch_add(1, std::memory_order_relaxed);
            }

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
                        hiom_.stats_.cold_hits.fetch_add(
                            1, std::memory_order_relaxed);
                        mirror_into_hot_with_offset(key, *cold_off);
                        return true;
                    }
                    hiom_.stats_.cold_fp_collisions.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    hiom_.stats_.cold_misses.fetch_add(
                        1, std::memory_order_relaxed);
                }
                return false;
            }

            // M0/M1 mode (no ColdTier): CCEH is authoritative. This
            // path is exercised by the legacy hit-mostly tests; not
            // the steady-state path once ColdTier is wired up.
            if (!viper_.get(key, value)) return false;
            mirror_into_hot_with_offset(key,
                                        viper_.hiom_peek_offset(key));
            hiom_.stats_.hot_warmups.fetch_add(
                1, std::memory_order_relaxed);
            return true;
        }

        bool remove(const K& key) {
            const bool ok = viper_.remove(key);
            if (!ok) return false;
            hiom_.hot_.remove(key_fingerprint(key));
            if (hiom_.cold_ != nullptr) {
                // Push remove through the buffer so it serializes with
                // earlier puts of the same key. Direct cold_.remove
                // would race with a still-buffered kPut for this fp,
                // and the flusher would then write the obsolete put
                // *after* the remove.
                const std::uint64_t seq = hiom_.commit_seq_.fetch_add(
                    1, std::memory_order_seq_cst) + 1;
                push_commit({CommitEntry::Op::kRemove,
                             {}, key_fingerprint64(key),
                             KVOffset{},  // unused for kRemove
                             HotTier::SlotRef{},
                             seq});
            }
            return true;
        }

        template <typename UpdateFn>
        bool update(const K& key, UpdateFn fn) {
            if (!viper_.update(key, std::move(fn))) return false;
            // Viper update is in-place; offset unchanged. Still push a
            // kPut so HotTier's visited bit re-fires (touch keeps the
            // entry "hot" for SIEVE) and the buffer's PINNED window
            // re-arms — harmless duplicate cold-tier upsert.
            mirror_write(key, CommitEntry::Op::kPut);
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
        bool verify_and_read_offset(const K& key, KVOffset off, V* value) {
            if (off.is_tombstone()) return false;
            K pm_key;
            V pm_val;
            if (!viper_.hiom_read_at_offset(off, &pm_key, &pm_val)) {
                return false;  // locked or version-torn
            }
            if (!(pm_key == key)) return false;  // fp collision
            *value = std::move(pm_val);
            return true;
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
            if (packed) {
                if (hiom_.cold_ != nullptr) {
                    // Buffered path: pin so the slot survives until
                    // the flusher writes the corresponding cold-tier
                    // entry. M4 Phase B: if the bucket is full of
                    // PINNED slots, drain a chunk synchronously and
                    // retry — the inline-flush turns some PINNED
                    // slots back to UNPINNED, freeing room.
                    const std::uint32_t fp32 = key_fingerprint(key);
                    ref = hiom_.hot_.upsert_pinned(fp32, *packed);
                    for (int attempt = 0;
                         !ref.valid && attempt < kMaxBackpressureRetries;
                         ++attempt) {
                        if (hiom_.try_inline_flush(kInlineFlushBatch) == 0) {
                            // Another thread is draining (or queue
                            // empty); nudge background flusher and
                            // yield briefly.
                            hiom_.flush_cv_.notify_one();
                            std::this_thread::sleep_for(
                                std::chrono::microseconds(50));
                        }
                        ref = hiom_.hot_.upsert_pinned(fp32, *packed);
                    }
                } else {
                    // No buffer; classic M0 mirror. Eviction is safe
                    // because Viper's CCEH still holds the offset.
                    hiom_.hot_.upsert(key_fingerprint(key), *packed);
                }
            }

            if (hiom_.cold_ != nullptr) {
                const std::uint64_t seq = hiom_.commit_seq_.fetch_add(
                    1, std::memory_order_seq_cst) + 1;
                push_commit({op, {}, key_fingerprint64(key), off, ref, seq});
                // M4 Phase B: if upsert_pinned failed (ref.valid==false),
                // the entry isn't in HotTier. Block until the commit
                // buffer is drained so it reaches ColdTier before this
                // put returns — otherwise reads of `key` during the
                // commit window would miss both tiers (CCEH retired
                // in M3 Phase D). Synchronous; only fires on the rare
                // bucket-full path.
                if (!ref.valid && hiom_.commit_buf_) {
                    while (hiom_.commit_buf_->size_hint() > 0) {
                        if (hiom_.try_inline_flush(kInlineFlushBatch) == 0) {
                            hiom_.flush_cv_.notify_one();
                            std::this_thread::sleep_for(
                                std::chrono::microseconds(50));
                        }
                    }
                }
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
        // per-thread ProducerToken (allocated lazily on first write).
        // Token allocation is the only contention point with the queue
        // here; subsequent pushes are near-SPSC.
        void push_commit(const CommitEntry& e) {
            if (!prod_tok_) {
                prod_tok_ = std::make_unique<moodycamel::ProducerToken>(
                    hiom_.commit_buf_->make_producer_token());
            }
            hiom_.commit_buf_->push(*prod_tok_, e);
            // Wake the flusher if we crossed the high watermark, so we
            // don't always wait the full kFlushIntervalMs of latency.
            if (hiom_.commit_buf_->size_hint() >= hiom_.fcfg_.high_watermark) {
                hiom_.flush_cv_.notify_one();
            }
        }

        HiOM& hiom_;
        typename ViperT::Client viper_;
        std::unique_ptr<moodycamel::ProducerToken> prod_tok_;
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
        return c;
    }

    HotTier& hot_tier() { return hot_; }
    ColdTier* cold_tier() { return cold_; }
    CommitBuffer* commit_buffer() { return commit_buf_.get(); }
    const Stats& stats() const { return stats_; }
    BlockBaseMap& base_map() { return base_map_; }

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
        if (flusher_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(flush_mu_);
                stop_ = true;
            }
            flush_cv_.notify_all();
            flusher_.join();
        }
        commit_buf_.reset();
    }

    // Block until the commit buffer is fully drained (all pending
    // writes have been applied to ColdTier and corresponding HotTier
    // pins released). Used by tests and graceful shutdown sequences;
    // not on the steady-state path.
    void flush_and_wait() {
        if (!commit_buf_) return;
        while (true) {
            flush_cv_.notify_all();
            // Caught up only when the queue is empty AND the flusher
            // is not mid-batch. Both flags must be observed clear in
            // sequence; a producer may push between them, so we recheck
            // a few times before declaring stable.
            if (commit_buf_->size_hint() == 0
                && !flushing_in_progress_.load(std::memory_order_acquire)) {
                bool stable = true;
                for (int i = 0; i < 4; ++i) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    if (commit_buf_->size_hint() != 0
                        || flushing_in_progress_.load(
                               std::memory_order_acquire)) {
                        stable = false;
                        break;
                    }
                }
                if (stable) return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // M4 Phase B: inline-flush back-pressure. Drain up to `target`
    // entries from the commit buffer synchronously on the calling
    // thread. Used by Client::mirror_write when upsert_pinned hits a
    // bucket-full-of-PINNED case; flushing some entries transitions
    // their slots to UNPINNED, freeing room for the retry.
    //
    // Returns the number of entries drained, or 0 if another thread
    // is already inline-flushing OR the background flusher is mid-
    // apply (caller should yield and retry). M4 Phase C: try_lock
    // on apply_mu_ to coordinate with drain_once; same lock means
    // strict cross-batch ordering of (fp64, seq) is preserved
    // because every apply pass drains the queue to empty.
    std::size_t try_inline_flush(std::size_t /*target ignored after Phase C*/) {
        if (!commit_buf_ || cold_ == nullptr) return 0;
        std::unique_lock<std::mutex> lk(apply_mu_, std::try_to_lock);
        if (!lk.owns_lock()) return 0;
        const std::size_t got = drain_to_empty_and_apply_locked();
        return got;
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
        std::atomic<std::uint64_t> last_block{kNoActiveBlock};
        std::atomic<bool> active{false};
    };
    std::array<ClientSlot, kMaxClientSlots> client_slots_{};

    // Reserve a slot. Returns the index, or kInvalidSlotIdx if all
    // slots are taken (Client falls back to "no tracking" — its
    // writes still go through, but they don't constrain the
    // checkpoint frontier).
    std::size_t reserve_client_slot() {
        for (std::size_t i = 0; i < kMaxClientSlots; ++i) {
            bool expected = false;
            if (client_slots_[i].active.compare_exchange_strong(
                    expected, true,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                client_slots_[i].last_block.store(
                    kNoActiveBlock, std::memory_order_release);
                return i;
            }
        }
        return kInvalidSlotIdx;
    }

    // Release a slot. Note: we do NOT reset last_block to kNoActiveBlock
    // synchronously here. The Client's pending pushes may still be in
    // the commit buffer at the moment of release; if we cleared
    // last_block, an immediate try_write_checkpoint would compute a
    // min that excludes this client's last block, and recovery would
    // miss the unflushed entries. Instead we keep last_block intact
    // and clear `active` only — min_active_writer_block skips
    // inactive slots, so once the buffer drains and the next checkpoint
    // fires, the slot no longer constrains the frontier. The slot is
    // immediately reusable; reserve_client_slot resets last_block
    // when it claims it.
    //
    // This means there's a brief window after release where a
    // checkpoint that fires before the released client's entries
    // drain could capture a stale-too-low frontier — but that just
    // means recovery scans a slightly larger range than necessary,
    // which is harmless (idempotent re-upserts).
    void release_client_slot(std::size_t idx) {
        if (idx >= kMaxClientSlots) return;
        client_slots_[idx].active.store(false, std::memory_order_release);
    }

    void note_client_block(std::size_t idx, std::uint64_t block_number) {
        if (idx >= kMaxClientSlots) return;
        client_slots_[idx].last_block.store(block_number,
                                            std::memory_order_release);
    }

    // Min over all active slots' last_block. Returns kNoActiveBlock
    // if no slot is active (or all are at their initial state). The
    // caller (try_write_checkpoint) then takes min(this, viper's
    // current_block_page_) to compose a safe frontier.
    //
    // Read order: last_block FIRST, then active. If a slot is in the
    // middle of being released (active=false, last_block stale), we
    // see stale last_block and active=false, and skip — the slot's
    // last value would have constrained the frontier, but since the
    // client is gone, its entries are either drained (no constraint
    // needed) or in the buffer (next non-skipped slot's last_block
    // covers the same range, OR no client survives → frontier falls
    // back to current_block_page_, which over-scans).
    std::uint64_t min_active_writer_block() const {
        std::uint64_t m = kNoActiveBlock;
        for (auto& s : client_slots_) {
            const auto b = s.last_block.load(std::memory_order_acquire);
            if (!s.active.load(std::memory_order_acquire)) continue;
            if (b < m) m = b;
        }
        return m;
    }
    // ----------------------------------------------------------------

    // M4 Phase C: drain the queue until try_drain returns 0 (i.e.
    // queue empty from this consumer's view), then sort by
    // (fp64 asc, seq asc), coalesce same-fp runs in apply_batch,
    // and apply. Caller must hold apply_mu_. Returns total drained.
    std::size_t drain_to_empty_and_apply_locked() {
        // Set flushing_in_progress_ BEFORE we start dequeuing. Otherwise
        // flush_and_wait can observe (size_hint==0, flushing==false) in
        // the gap between try_drain's last successful dequeue and the
        // store(true) below — even though apply_batch hasn't run yet —
        // and incorrectly declare the queue "stable", returning before
        // ColdTier sees the drained entries.
        flushing_in_progress_.store(true, std::memory_order_release);
        std::vector<CommitEntry> batch;
        std::size_t total = 0;
        constexpr std::size_t kPerCall = 1024;
        while (true) {
            const std::size_t prev = batch.size();
            const std::size_t got = commit_buf_->try_drain(
                *flusher_consumer_tok_, batch, kPerCall);
            if (got == 0) break;
            total += got;
            (void)prev;
        }
        if (total == 0) {
            flushing_in_progress_.store(false, std::memory_order_release);
            return 0;
        }
        std::stable_sort(batch.begin(), batch.end(),
                         [](const CommitEntry& a, const CommitEntry& b) {
                             if (a.fp64 != b.fp64) return a.fp64 < b.fp64;
                             return a.seq < b.seq;
                         });
        apply_batch(batch);
        stats_.commits_flushed.fetch_add(total, std::memory_order_relaxed);
        flushing_in_progress_.store(false, std::memory_order_release);
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
            // batch[i..j) is one fp64 run, sorted by seq asc.
            // Walk descending and pick the first kRemove (final by
            // intent) or alive-and-fp-matching kPut.
            const CommitEntry* winner = nullptr;
            for (std::size_t k = j; k > i; --k) {
                const CommitEntry& e = batch[k - 1];
                if (e.op == CommitEntry::Op::kRemove) {
                    winner = &e;
                    break;
                }
                // kPut: the slot must be occupied AND still hold a
                // key that hashes to e.fp64. hiom_get_slot_key
                // returns false if free_slots is set; the read of
                // the key itself is safe because Viper persists the
                // key bytes *before* clearing the free_slots bit.
                // The fp64 check is what filters out the case where
                // a freed slot has been re-allocated to a different
                // key under heavy update concurrency — without it,
                // ColdTier would point at a slot whose data is for
                // another key.
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
            cold_->bulk_upsert(puts);
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
        // M4 Phase E: vpage_frontier is the min over (a) Viper's
        // current next-to-claim block and (b) the lowest block any
        // active HiOM client has recently written into. Capturing
        // only (a) is unsafe with concurrent writers — when client_X
        // is still pushing into block 30 while the global frontier
        // has advanced to block 50, recovery's tail-scan starts at
        // block 49 and silently drops client_X's unflushed entries.
        // Using the min ensures every block with potentially
        // unflushed entries is in the tail-scan range.
        const std::uint64_t client_min = min_active_writer_block();
        const std::uint64_t viper_next
            = KVOffset{viper_.hiom_vpage_frontier()}.block_number;
        const std::uint64_t safe_block = std::min(client_min, viper_next);
        rec.vpage_frontier = KVOffset{
            static_cast<viper::block_size_t>(safe_block), 0, 0
        }.offset;
        rec.cold_size = (cold_ != nullptr) ? cold_->approx_size() : 0;
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
                frontier_block
                    = KVOffset{rec->vpage_frontier}.block_number;
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
                        cold_->upsert(key_fingerprint64(key), off);
                        ++local;
                    });
                total.fetch_add(local, std::memory_order_relaxed);
            });
        }
        for (auto& w : workers) w.join();
        return total.load(std::memory_order_relaxed);
    }

    // Single-cycle drain helper, shared between the flusher loop and
    // the destructor's final cleanup pass. Returns the count drained.
    // M4 Phase C: takes apply_mu_ (blocking) so background and inline
    // flushers serialize; whoever holds it drains the queue to
    // empty, then sorts (fp64, seq) and coalesces same-fp runs.
    std::size_t drain_once(std::size_t /*max ignored after Phase C*/) {
        if (!commit_buf_ || cold_ == nullptr) return 0;
        std::lock_guard<std::mutex> lk(apply_mu_);
        return drain_to_empty_and_apply_locked();
    }

    // Background flusher: wakes on timer or high-watermark notify and
    // drains the commit buffer in batches.
    void flusher_loop() {
        std::unique_lock<std::mutex> lk(flush_mu_);
        while (!stop_) {
            flush_cv_.wait_for(lk, fcfg_.interval, [this] {
                return stop_
                    || (commit_buf_ && commit_buf_->size_hint()
                                       >= fcfg_.high_watermark);
            });
            lk.unlock();
            // Drain in chunks until under the watermark again, so a
            // sudden spike doesn't wait kFlushIntervalMs per chunk.
            std::size_t drained;
            do {
                drained = drain_once(fcfg_.batch_max);
            } while (drained > 0
                     && commit_buf_->size_hint() >= fcfg_.high_watermark);
            lk.lock();
        }
    }

    ViperT& viper_;
    HotTier hot_;
    ColdTier* cold_;
    BlockBaseMap base_map_;
    Stats stats_;
    FlusherConfig fcfg_;

    std::unique_ptr<CommitBuffer> commit_buf_;
    std::unique_ptr<moodycamel::ConsumerToken> flusher_consumer_tok_;
    std::thread flusher_;
    std::mutex flush_mu_;
    std::condition_variable flush_cv_;
    bool stop_{false};
    std::atomic<bool> flushing_in_progress_{false};

    // M4 Phase C: drain+apply serialization. A single mutex covers
    // both the background flusher's drain_once and the writer-side
    // try_inline_flush, so concurrent same-fp upserts can't race in
    // the cold tier. Whoever holds it drains the queue to empty (so
    // each apply pass sees every entry that was already pushed and
    // can sort/coalesce by (fp64, seq) for strict happens-before
    // correctness across producer threads).
    std::mutex apply_mu_;

    // M4 Phase C: monotonic stamp source for CommitEntry.seq. Bumped
    // by HiOM::Client at push time. Distinct from `seq_` (M5
    // checkpoint generation, bumped per-checkpoint).
    std::atomic<std::uint64_t> commit_seq_{0};

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
