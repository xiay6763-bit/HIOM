#pragma once

// HiOM commit buffer (M3) — region-sharded queue of (op, fp64, offset,
// hot_slot) entries that the background flusher drains and applies to
// ColdTier in batches.
//
// Why a buffer at all (paper §2.6 / §M3): without batching, each
// successful client write incurs a full ColdTier upsert — a small
// (~16 B) PM write + ADR fence. Across N concurrent clients this
// consumes the Optane write-buffer queue and limits aggregate write
// throughput. By deferring ColdTier writes through this buffer, the
// flusher can sort entries by destination bucket and amortize the
// fence across many entries.
//
// Concurrency model (M3 follow-up #2 Step 2, 2026-05-14):
//   - The buffer is sharded into kNumLanes = 8 cache-line-isolated
//     queues. Routing is by ColdTier region (top 5 bits of fp64);
//     four regions per lane. *Same fp64 always lands in the same
//     lane*, which is the load-bearing property: the apply path's
//     (fp64, seq) sort + winner-picker only needs to see all entries
//     for a given fp inside one batch, and a single lane's drain
//     guarantees that.
//   - Each (Client, lane) pair owns a moodycamel::ProducerToken,
//     allocated lazily on the first push. Producers see near-SPSC
//     enqueue performance per token and the cross-lane contention
//     that the single-queue design suffered from is split kNumLanes
//     ways.
//   - A nonempty_mask (one bit per lane) lets the flusher skip
//     empty lanes without per-lane size_approx() probes. To avoid
//     turning the mask into the new contention point, push() only
//     hits the atomic OR when its fetch_add observes a 0→1
//     transition (test-and-set pattern). Drain clears the bit when
//     it observes the lane drained to empty.
//   - In Step 2 there is still a single flusher; it walks the mask
//     and drains every non-empty lane in one pass. Step 3 will
//     partition lane ownership across multiple flusher threads.
//
// Crash semantics: the buffer is purely volatile DRAM. On crash, all
// un-flushed entries are lost — but the corresponding PM data and
// any committed ColdTier upserts are still durable, and M5 / M6's
// checkpoint + tail-scan recover the unflushed window.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "concurrentqueue.h"

#include "viper/cceh.hpp"  // viper::KeyValueOffset
#include "viper/hiom/hot_tier.hpp"

namespace viper::hiom {

struct CommitEntry {
    enum class Op : std::uint8_t {
        kPut = 0,     // ColdTier upsert with `off`
        kRemove = 1,  // ColdTier remove (off ignored, kept for layout)
    };

    Op op{Op::kPut};
    std::uint8_t pad0[7]{};       // align fp64 to 8 B; reserved
    std::uint64_t fp64{0};
    viper::KeyValueOffset off{};
    HotTier::SlotRef hot_slot{};  // unpin target after flush; valid==false skipped
    // Monotonic stamp packed as (client_local_seq << 16) | slot_idx,
    // assigned by HiOM::Client at push time. Apply path sorts by
    // (fp64, seq) and coalesces same-fp runs, applying only the
    // highest-seq entry. Semantics (M3 follow-up #2 Step 1, 2026-05-14):
    //   - intra-Client: client_local_seq is bumped per push, so two
    //     pushes from the same Client carry strictly monotone seq.
    //   - cross-Client: no real-time ordering guarantee. The apply
    //     path's HotTier-truth fast path picks the canonical winner
    //     by reading the slot's current (fp32, packed_off); seq is
    //     only consulted by the rare fallback walk, where slot_idx
    //     in the low 16 bits acts as a deterministic tiebreaker.
    // seq=0 is the default-constructed sentinel; client_local_seq
    // is pre-incremented before packing, so the first assigned seq
    // is at least (1 << 16) = 65536.
    std::uint64_t seq{0};
};
static_assert(sizeof(CommitEntry) <= 64,
              "CommitEntry should fit comfortably in a cache line");

class CommitBuffer {
  public:
    static constexpr std::size_t kNumLanes = 8;
    static_assert((kNumLanes & (kNumLanes - 1)) == 0,
                  "kNumLanes must be a power of two");

    // Map fp64 → lane_id. Same fp64 always maps to the same lane,
    // which guarantees apply_batch sees every entry for that fp in
    // a single drained batch. ColdTier uses (fp >> 59) for its
    // region id (top 5 bits → 32 regions); we group 4 regions per
    // lane (kNumLanes = 8). The shift `59 + log2(32/kNumLanes)`
    // generalises to any kNumLanes that divides 32.
    static constexpr std::size_t lane_of_fp64(std::uint64_t fp64) {
        constexpr std::size_t kRegionsPerLane = 32 / kNumLanes;
        constexpr std::size_t kLaneShift
            = 59 + (kRegionsPerLane > 1 ? __builtin_ctz(kRegionsPerLane) : 0);
        return static_cast<std::size_t>(fp64 >> kLaneShift)
             & (kNumLanes - 1);
    }

    CommitBuffer() = default;
    CommitBuffer(const CommitBuffer&) = delete;
    CommitBuffer& operator=(const CommitBuffer&) = delete;

    // Producer/consumer tokens are objects, not ints; callers move them.
    // Each (Client, lane_id) pair allocates one ProducerToken on first
    // push. Each lane has one long-lived ConsumerToken (held by the
    // flusher).
    moodycamel::ProducerToken make_producer_token(std::size_t lane_id) {
        return moodycamel::ProducerToken{lanes_[lane_id].q};
    }
    moodycamel::ConsumerToken make_consumer_token(std::size_t lane_id) {
        return moodycamel::ConsumerToken{lanes_[lane_id].q};
    }

    // Single-entry enqueue from a client thread. ProducerToken should
    // be the caller's per-(thread, lane) token. The fetch_add is the
    // sole atomic on the steady-state push path; the OR on
    // nonempty_mask_ only fires on the rare 0→1 transition (per
    // user's review note).
    void push(moodycamel::ProducerToken& tok, std::size_t lane_id,
              const CommitEntry& e) {
        Lane& lane = lanes_[lane_id];
        const std::size_t before
            = lane.approx_size.fetch_add(1, std::memory_order_relaxed);
        lane.q.enqueue(tok, e);
        if (before == 0) {
            // Test-and-set: only contend on the global mask when the
            // lane transitions empty → non-empty. Under steady-state
            // throughput the lane stays non-empty between flushes
            // and producers skip this entirely.
            nonempty_mask_.fetch_or(1ull << lane_id,
                                    std::memory_order_release);
        }
    }

    // Drain up to `max` entries from a single lane (single-consumer per
    // lane in Step 2's design — the flusher serializes across lanes;
    // Step 3 will give each flusher its own subset). Returns the
    // number of entries actually drained. Callers pass a per-lane
    // ConsumerToken acquired via make_consumer_token(lane_id).
    std::size_t try_drain_lane(moodycamel::ConsumerToken& ctok,
                               std::size_t lane_id,
                               std::vector<CommitEntry>& out,
                               std::size_t max) {
        if (max == 0) return 0;
        Lane& lane = lanes_[lane_id];
        const std::size_t hint
            = lane.approx_size.load(std::memory_order_relaxed);
        const std::size_t cap = std::min(max, hint + 64);
        if (cap == 0) return 0;
        const std::size_t prev_size = out.size();
        out.resize(prev_size + cap);
        const std::size_t got = lane.q.try_dequeue_bulk(
            ctok, out.data() + prev_size, cap);
        out.resize(prev_size + got);
        if (got > 0) {
            const std::size_t before = lane.approx_size.fetch_sub(
                got, std::memory_order_relaxed);
            if (before == got) {
                // We drained the lane to empty (as far as this consumer
                // observed). Clear the mask bit so the flusher skips
                // this lane next pass; a concurrent push will re-set
                // the bit via its own 0→1 transition check. The
                // race-safety relies on linearizable atomics — see
                // header comment for the proof sketch.
                nonempty_mask_.fetch_and(~(1ull << lane_id),
                                         std::memory_order_release);
            }
        }
        return got;
    }

    // Bitmask of lanes that may have entries. The drain side iterates
    // only set bits; the flusher's wake-up path uses size_hint() (the
    // total) to decide whether to fire at all.
    std::uint64_t nonempty_mask() const {
        return nonempty_mask_.load(std::memory_order_acquire);
    }

    // Per-lane and total approximate size. The total path walks all
    // kNumLanes atomics; called only on flusher wake-up checks (every
    // ~5 ms by default), so the cost is amortised.
    std::size_t size_hint(std::size_t lane_id) const {
        return lanes_[lane_id].approx_size.load(std::memory_order_relaxed);
    }
    std::size_t size_hint() const {
        std::size_t total = 0;
        for (auto& lane : lanes_) {
            total += lane.approx_size.load(std::memory_order_relaxed);
        }
        return total;
    }

  private:
    struct alignas(64) Lane {
        moodycamel::ConcurrentQueue<CommitEntry> q;
        std::atomic<std::size_t> approx_size{0};
        // alignas(64) makes sizeof(Lane) a multiple of 64 (C++ rule:
        // size is a multiple of alignment), so adjacent Lanes are on
        // disjoint cache lines without manual padding.
    };
    std::array<Lane, kNumLanes> lanes_;
    std::atomic<std::uint64_t> nonempty_mask_{0};
};

}  // namespace viper::hiom
