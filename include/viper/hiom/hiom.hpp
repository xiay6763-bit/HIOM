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
    };

    // Tunables for the background flusher. Defaults are chosen to
    // balance latency (drain in <10 ms steady state) against per-batch
    // overhead. Caller can override at construction.
    struct FlusherConfig {
        std::chrono::milliseconds interval{std::chrono::milliseconds(5)};
        std::size_t high_watermark{1024};   // wake on size_hint() >= this
        std::size_t batch_max{8192};        // drain at most this many per cycle
    };

    HiOM(ViperT& viper, std::size_t hot_buckets_pow2,
         ColdTier* cold = nullptr,
         FlusherConfig fcfg = FlusherConfig{})
        : viper_(viper),
          hot_(hot_buckets_pow2),
          cold_(cold),
          base_map_{},  // M0/M2: all zero, single region 0
          fcfg_(fcfg)
    {
        if (cold_ != nullptr) {
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
        Client(HiOM& hiom, typename ViperT::Client viper_client)
            : hiom_(hiom), viper_(std::move(viper_client)) {}

        bool put(const K& key, const V& value) {
            // Step 1: viper does the PM-side write (data + bitset persisted)
            // and updates its own CCEH. Persistence ordering is unchanged
            // from the original Viper put path (viper.hpp ~1037-1054).
            if (!viper_.put(key, value)) return false;

            // Step 2: peek the KVOffset Viper just installed and mirror
            // into HotTier (PINNED) and the commit buffer / ColdTier.
            // All of this happens *after* PM persistence so a crash
            // never leaves either tier pointing at non-persisted data;
            // the volatile commit buffer is allowed to be lost on crash
            // because Viper's CCEH (still the M2/M3 safety net) holds
            // the same offset and recovery rebuilds from VPage scan.
            mirror_write(key, CommitEntry::Op::kPut);
            return true;
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
                push_commit({CommitEntry::Op::kRemove,
                             {}, key_fingerprint64(key),
                             KVOffset{},  // unused for kRemove
                             HotTier::SlotRef{}});
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

            HotTier::SlotRef ref{};
            const auto [block, page, slot] = off.get_offsets();
            auto packed = encode(block,
                                 static_cast<std::uint8_t>(page),
                                 static_cast<std::uint16_t>(slot),
                                 route_to_region_default(),
                                 hiom_.base_map_);
            if (packed) {
                if (hiom_.cold_ != nullptr) {
                    // Buffered path: pin so the slot survives until the
                    // flusher writes the corresponding cold-tier entry.
                    ref = hiom_.hot_.upsert_pinned(
                        key_fingerprint(key), *packed);
                } else {
                    // No buffer; classic M0 mirror. Eviction is safe
                    // because Viper's CCEH still holds the offset.
                    hiom_.hot_.upsert(key_fingerprint(key), *packed);
                }
            }

            if (hiom_.cold_ != nullptr) {
                push_commit({op, {}, key_fingerprint64(key), off, ref});
            }
        }

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
    };

    Client get_client() { return Client{*this, viper_.get_client()}; }

    HotTier& hot_tier() { return hot_; }
    ColdTier* cold_tier() { return cold_; }
    CommitBuffer* commit_buffer() { return commit_buf_.get(); }
    const Stats& stats() const { return stats_; }
    BlockBaseMap& base_map() { return base_map_; }

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

  private:
    // Single-cycle drain helper, shared between the flusher loop and
    // the destructor's final cleanup pass. Returns the count drained.
    std::size_t drain_once(std::size_t max) {
        if (!commit_buf_ || cold_ == nullptr) return 0;
        std::vector<CommitEntry> batch;
        const std::size_t got = commit_buf_->try_drain(
            *flusher_consumer_tok_, batch, max);
        if (got == 0) return 0;
        flushing_in_progress_.store(true, std::memory_order_release);
        std::sort(batch.begin(), batch.end(),
                  [](const CommitEntry& a, const CommitEntry& b) {
                      return a.fp64 < b.fp64;
                  });
        for (const auto& e : batch) {
            if (e.op == CommitEntry::Op::kPut) {
                cold_->upsert(e.fp64, e.off);
            } else {
                cold_->remove(e.fp64);
            }
            if (e.hot_slot.valid) hot_.unpin(e.hot_slot);
        }
        stats_.commits_flushed.fetch_add(got, std::memory_order_relaxed);
        flushing_in_progress_.store(false, std::memory_order_release);
        return got;
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
};

}  // namespace viper::hiom
