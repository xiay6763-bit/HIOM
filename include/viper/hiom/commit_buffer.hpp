#pragma once

// HiOM commit buffer (M3) — per-thread queue of (op, fp64, offset, hot_slot)
// entries that the background flusher drains and applies to ColdTier in
// batches.
//
// Why a buffer at all (paper §2.6 / §M3): without batching, each
// successful client write incurs a full ColdTier upsert — a small
// (~16 B) PM write + ADR fence. Across N concurrent clients this
// consumes the Optane write-buffer queue and limits aggregate write
// throughput. By deferring ColdTier writes through this buffer, the
// flusher can sort entries by destination bucket and amortize the
// fence across many entries.
//
// Concurrency model (M3 scope):
//   - Each HiOM Client owns a `moodycamel::ProducerToken` and pushes
//     into the shared queue. ProducerTokens give the queue
//     near-SPSC performance per producer; the flusher reads from
//     all producers via a single ConsumerToken.
//   - Bounded resource use: queue has no hard cap (moodycamel grows
//     in 32-block chunks); we track an approximate size for flush
//     triggers. Inline-flush back-pressure is M4 work.
//
// Crash semantics: the buffer is purely volatile DRAM. On crash, all
// un-flushed entries are lost — but the corresponding PM data and
// the Viper CCEH safety-net entry are still durable, so M2/M3
// recovery from CCEH or VPage scan still finds them. M4's PINNED
// state machine and M5's A/B checkpoint formalize the durability
// contract; in M3 we rely on CCEH as the recovery source.

#include <algorithm>
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
    // M4 Phase C: monotonic stamp from HiOM::commit_seq_, taken at
    // push time. Apply path sorts by (fp64, seq) and coalesces same-fp
    // runs, applying only the highest-seq entry. seq=0 is the
    // default-constructed sentinel, never assigned by Client paths
    // (commit_seq_.fetch_add(1) + 1 starts at 1).
    std::uint64_t seq{0};
};
static_assert(sizeof(CommitEntry) <= 64,
              "CommitEntry should fit comfortably in a cache line");

class CommitBuffer {
  public:
    CommitBuffer() = default;
    CommitBuffer(const CommitBuffer&) = delete;
    CommitBuffer& operator=(const CommitBuffer&) = delete;

    // Producer/consumer tokens are objects, not ints; callers move them.
    moodycamel::ProducerToken make_producer_token() {
        return moodycamel::ProducerToken{q_};
    }
    moodycamel::ConsumerToken make_consumer_token() {
        return moodycamel::ConsumerToken{q_};
    }

    // Single-entry enqueue from a client thread. ProducerToken should
    // be the caller's per-thread token to avoid contending on the
    // queue's implicit-producer lookup.
    void push(moodycamel::ProducerToken& tok, const CommitEntry& e) {
        // moodycamel::ConcurrentQueue::enqueue(token, value) is
        // lock-free and strongly bounded by tok-local block allocation.
        q_.enqueue(tok, e);
        approx_size_.fetch_add(1, std::memory_order_relaxed);
    }

    // Drain up to `max` entries into `out` (appended). Returns the
    // number of entries actually drained. Designed to be called from
    // a single flusher thread holding the ConsumerToken. `max` is
    // clamped to the current size_hint() (plus a small slack for
    // races) to avoid pathological vector resizes when callers pass
    // numeric_limits<size_t>::max() for "drain everything".
    std::size_t try_drain(moodycamel::ConsumerToken& ctok,
                          std::vector<CommitEntry>& out,
                          std::size_t max) {
        if (max == 0) return 0;
        const std::size_t hint = approx_size_.load(std::memory_order_relaxed);
        const std::size_t cap = std::min(max, hint + 64);
        if (cap == 0) return 0;
        const std::size_t prev_size = out.size();
        out.resize(prev_size + cap);
        const std::size_t got = q_.try_dequeue_bulk(
            ctok, out.data() + prev_size, cap);
        out.resize(prev_size + got);
        if (got > 0) {
            approx_size_.fetch_sub(got, std::memory_order_relaxed);
        }
        return got;
    }

    std::size_t size_hint() const {
        return approx_size_.load(std::memory_order_relaxed);
    }

  private:
    moodycamel::ConcurrentQueue<CommitEntry> q_;
    std::atomic<std::size_t> approx_size_{0};
};

}  // namespace viper::hiom
