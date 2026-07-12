// Backpressure regression test for the bounded pending-block ring (Claim 5A).
//
// The pending ring (PendingBlocks, kWindow = 2^16 slots) tracks blocks with
// commit-buffer entries not yet durably applied to ColdTier. It used to
// std::abort() with "pending-block window overflow" when a producer tried to
// register a block whose ring residue was still occupied by an OLDER block the
// flusher had not yet retired — i.e. when the flusher fell >= kWindow blocks
// behind. That turned a legitimate "consumer is behind" condition into a fatal
// protocol error, so a fast multi-threaded prefill of > 65536 blocks (a 10M
// K8/V200 YCSB prefill = ~87.7k blocks) crashed deterministically during the
// insert storm.
//
// The fix makes pending_register BACKPRESSURE instead: on a residue collision
// it wakes the flushers and WAITS for the slot to drain, then retries.
//
// This test proves the fix TWO ways:
//   (A) Deterministic, via direct pending_register/pending_retire test hooks:
//       thread T2 registers block (B + kWindow) whose residue collides with a
//       still-pending block B; T2 MUST block, the main thread then retires B,
//       and T2 must then complete. Asserts T2 did not finish early (it really
//       waited), pending_ring_stalls incremented, and no abort. This is
//       independent of insert scheduling, so it always exercises the path.
//   (B) End-to-end: a real multi-threaded insert of > kWindow blocks completes
//       without abort and every key is present. (Whether it stalls depends on
//       flusher timing; (A) is the load-bearing assertion.)
//
// PM artefacts under /pmem0/hiom/backpressure_test — prefix-guarded cleanup.

#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

// A big value so records/block matches the K8/V200 geometry that actually
// crashed (~102 records/block => the 65536-block ring window is crossed at
// ~6.7M records, not the ~90M a tiny <u64,u64> record would need).
struct BigVal {
    std::uint64_t id;
    unsigned char pad[200];
    bool operator==(const BigVal& o) const { return id == o.id; }
    bool operator!=(const BigVal& o) const { return id != o.id; }
};

using KeyT = std::uint64_t;
using ValueT = BigVal;
using ViperT = viper::Viper<KeyT, ValueT>;
using HiOMT = viper::hiom::HiOM<KeyT, ValueT>;

inline ValueT make_val(std::uint64_t id) {
    ValueT v{};
    v.id = id;
    return v;
}

constexpr const char* kRoot = "/pmem0/hiom/backpressure_test";
constexpr const char* kPrefix = "/pmem0/hiom/backpressure_test/";
constexpr const char* kPoolDir = "/pmem0/hiom/backpressure_test/pool";
constexpr const char* kColdFile = "/pmem0/hiom/backpressure_test/cold/cold.bin";
constexpr const char* kChkptFile = "/pmem0/hiom/backpressure_test/chkpt/chkpt.bin";

void guarded_cleanup() {
    if (std::string(kPoolDir).find(kPrefix) != 0) {
        std::cerr << "Refusing to clean unfamiliar path\n";
        std::exit(2);
    }
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    std::filesystem::create_directories(std::string(kRoot) + "/cold", ec);
    std::filesystem::create_directories(std::string(kRoot) + "/chkpt", ec);
}

// ---- (A) deterministic collision via direct test hooks --------------------
int test_deterministic_backpressure(HiOMT& hiom) {
    std::cout << "\n[A] deterministic residue-collision backpressure...\n";
    const std::uint64_t W = HiOMT::pending_window_for_test();
    const std::uint64_t B = 5;              // arbitrary low block
    const std::uint64_t B2 = B + W;         // SAME residue mod W => collides

    // Occupy slot residue(B) with block B (count 1). The next register of B2
    // must find the slot tagged by B (!= B2) and back off (wait).
    hiom.pending_register_for_test(B);

    std::atomic<bool> t2_entered{false};
    std::atomic<bool> t2_done{false};
    std::thread t2([&] {
        t2_entered.store(true, std::memory_order_release);
        // This must BLOCK until the main thread retires B, because B2's residue
        // slot is held by B. If the old abort path were still here, this would
        // std::abort() the process instead.
        hiom.pending_register_for_test(B2);
        t2_done.store(true, std::memory_order_release);
    });

    // Wait for t2 to reach the register call, then give it time to prove it is
    // genuinely blocked (not merely slow to start).
    while (!t2_entered.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (t2_done.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
            "[A] FAIL: registering B2 (residue-collides with pending B) did "
            "NOT block — backpressure not engaged.\n");
        t2.join();
        return 1;
    }
    std::cout << "[A]   B2 register correctly BLOCKED on the collision.\n";

    // Retire B: frees the slot residue and must wake t2, which then re-CASes B2
    // into the now-free slot and returns.
    hiom.pending_retire_for_test(B);

    // t2 must finish promptly now.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!t2_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr,
                "[A] FAIL: B2 register did not resume within 5s after B was "
                "retired — missed wakeup.\n");
            std::abort();  // t2 is stuck; abort so the test fails loudly
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    t2.join();

    const std::uint64_t stalls = hiom.pending_ring_stalls_for_test();
    if (stalls == 0) {
        std::fprintf(stderr,
            "[A] FAIL: pending_ring_stalls==0 — the collision path did not "
            "record a stall.\n");
        return 1;
    }
    // Clean up the ring: retire B2 so the pool teardown sees an empty ring.
    hiom.pending_retire_for_test(B2);
    std::cout << "[A] PASS: B2 blocked, resumed after retire(B), stalls="
              << stalls << ".\n";
    return 0;
}

// ---- (B) end-to-end multi-threaded insert completes without abort ---------
int test_end_to_end_insert(HiOMT& hiom, std::size_t N, int nthreads) {
    std::cout << "\n[B] end-to-end insert of " << N << " keys across "
              << nthreads << " threads (must complete, no abort)...\n";
    {
        std::vector<std::thread> writers;
        std::atomic<bool> failed{false};
        for (int t = 0; t < nthreads; ++t) {
            writers.emplace_back([t, nthreads, N, &hiom, &failed] {
                auto cl = hiom.get_client();
                for (std::uint64_t k = t + 1; k <= N;
                     k += static_cast<std::uint64_t>(nthreads)) {
                    if (!cl.put(k, make_val(k), /*assume_new=*/true)) {
                        std::fprintf(stderr, "FAIL: put(%llu) returned false\n",
                                     (unsigned long long)k);
                        failed.store(true);
                        return;
                    }
                }
            });
        }
        for (auto& w : writers) w.join();
        if (failed.load()) return 1;
    }
    hiom.flush_and_wait();

    std::cout << "[B]   insert complete (NO abort). pending_ring_stalls="
              << hiom.stats().pending_ring_stalls.load()
              << " wait_ns=" << hiom.stats().pending_ring_wait_ns.load()
              << ".\n";

    auto cl = hiom.get_client();
    std::size_t missing = 0;
    for (std::uint64_t k = 1; k <= N; ++k) {
        ValueT got;
        if (!cl.get(k, &got) || got != make_val(k)) {
            if (missing < 10) {
                std::fprintf(stderr, "  MISSING/mismatch key=%llu\n",
                             (unsigned long long)k);
            }
            ++missing;
        }
    }
    if (missing != 0) {
        std::fprintf(stderr, "[B] FAIL: %zu keys missing/mismatched.\n", missing);
        return 1;
    }
    std::cout << "[B] PASS: all " << N << " keys present, no abort.\n";
    return 0;
}

}  // namespace

int main() {
    std::cout << "=== HiOM pending-ring backpressure test ===" << std::endl;
    guarded_cleanup();

    const std::size_t N =
        (std::getenv("BP_N") != nullptr)
            ? std::strtoull(std::getenv("BP_N"), nullptr, 10)
            : 8'000'000;  // > 65536 blocks at ~102 rec/block
    const int nthreads =
        (std::getenv("BP_THREADS") != nullptr)
            ? std::atoi(std::getenv("BP_THREADS"))
            : 24;

    constexpr std::size_t kPoolSize = 16ull * 1024 * 1024 * 1024;  // 16 GiB
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    const auto sz = viper::hiom::ColdTier::sizing_for(N + 1);
    auto cold = viper::hiom::ColdTier::create(kColdFile, sz.main_buckets,
                                              sz.overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kChkptFile);

    HiOMT::FlusherConfig fcfg;
    fcfg.num_flushers =
        (std::getenv("BP_FLUSHERS") != nullptr)
            ? static_cast<std::size_t>(std::atoi(std::getenv("BP_FLUSHERS")))
            : 2;
    HiOMT::CheckpointConfig ccfg;
    ccfg.cadence_entries = 4096;

    HiOMT hiom(*viper_db, std::size_t(1) << 21, cold.get(), fcfg, chkpt.get(),
               ccfg);

    if (int rc = test_deterministic_backpressure(hiom); rc != 0) return rc;
    if (int rc = test_end_to_end_insert(hiom, N, nthreads); rc != 0) return rc;

    std::cout << "\nPASS: pending-ring backpressure verified (deterministic "
                 "collision + end-to-end insert)." << std::endl;
    return 0;
}
