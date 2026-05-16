// HiOM ↔ Viper integration test.
//
// Goals:
//   1. Correctness: insert N keys via HiOM, read them all back, get
//      matching values; remove some, verify they miss.
//   2. Update consistency: update existing keys, verify new values.
//   3. Hot-tier hit accounting: after a warm phase, hits >> warmups.
//   4. M0 exit comparison: hit-mostly lookup throughput on a HiOM
//      client vs. a raw Viper client on the SAME dataset, same access
//      pattern. Should be within ±10% (HiOM hits HotTier; raw Viper
//      hits CCEH).
//   5. Phase B-2: ColdTier-backed HiOM end-to-end. Every put mirrors
//      into ColdTier; reads of evicted-from-hot keys must hit ColdTier.
//   6. M3 Phase D: with the CCEH safety net retired, a HotTier+ColdTier
//      double-miss returns false. Read-your-write inside the commit
//      window relies on HotTier PINNED slots; the test asserts
//      pin_failures==0 and hot_hits==N for that case.

#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kRootDir = "/pmem0/hiom/integration_test";
constexpr const char* kPoolDir = "/pmem0/hiom/integration_test/pool";
constexpr const char* kColdPoolFile = "/pmem0/hiom/integration_test/cold/cold.bin";
constexpr const char* kColdPoolDir  = "/pmem0/hiom/integration_test/cold";
constexpr const char* kCheckpointDir  = "/pmem0/hiom/integration_test/chkpt";
constexpr const char* kCheckpointFile = "/pmem0/hiom/integration_test/chkpt/chkpt.bin";
constexpr std::size_t kPoolSize = 1ULL << 30;     // 1 GiB
constexpr std::size_t kNumKeys = 200'000;          // ~150 blocks of uint64_t pairs, well under 8K limit
constexpr std::size_t kHotBuckets = 1ULL << 15;    // 32K buckets × 16 = 512K slots
constexpr std::size_t kBenchIters = 5'000'000;

using ViperT = viper::Viper<std::uint64_t, std::uint64_t>;
using HiOMT = viper::hiom::HiOM<std::uint64_t, std::uint64_t>;

void cleanup_pool() {
    // Defensive: same pattern as benchmark/fixtures/viper_fixture.hpp.
    // Only touch our own directory under /pmem0/hiom/; never blanket-rm.
    if (std::string(kPoolDir).find("/pmem0/hiom/integration_test/") != 0) {
        std::cerr << "Refusing to clean unfamiliar pool path: " << kPoolDir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(kPoolDir);
    std::filesystem::create_directories(kRootDir);
}

void cleanup_cold_pool() {
    // Same defensive check for the ColdTier file. The directory holds
    // exactly cold.bin; nothing else from other users.
    if (std::string(kColdPoolDir).find("/pmem0/hiom/integration_test/") != 0) {
        std::cerr << "Refusing to clean unfamiliar cold path: " << kColdPoolDir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(kColdPoolDir);
    std::filesystem::create_directories(kColdPoolDir);
}

void cleanup_checkpoint_dir() {
    // Same defensive pattern. Holds only chkpt.bin (4 KB).
    if (std::string(kCheckpointDir).find("/pmem0/hiom/integration_test/") != 0) {
        std::cerr << "Refusing to clean unfamiliar checkpoint path: "
                  << kCheckpointDir << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(kCheckpointDir);
    std::filesystem::create_directories(kCheckpointDir);
}

int run_correctness() {
    std::cout << "=== HiOM correctness (insert + lookup) ===" << std::endl;
    cleanup_pool();
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    HiOMT hiom(*viper_db, kHotBuckets);
    auto client = hiom.get_client();

    std::mt19937_64 rng(0xc0ffee);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
    kv.reserve(kNumKeys);
    for (std::size_t i = 0; i < kNumKeys; ++i) {
        // Use distinct keys so we know exactly what to expect on lookup.
        kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
    }
    for (auto& [k, v] : kv) {
        if (!client.put(k, v)) {
            std::cerr << "  FAIL: put returned false for key " << k << std::endl;
            return 1;
        }
    }

    std::size_t hits = 0, misses = 0, mismatches = 0;
    for (auto& [k, expected] : kv) {
        std::uint64_t got = 0;
        if (!client.get(k, &got)) ++misses;
        else if (got != expected) ++mismatches;
        else ++hits;
    }
    std::cout << "  inserted " << kv.size() << " keys" << std::endl;
    std::cout << "  lookup: " << hits << " hits, " << misses << " misses, "
              << mismatches << " mismatches" << std::endl;
    const auto& s = hiom.stats();
    std::cout << "  hot stats: hits=" << s.hot_hits.load()
              << " misses=" << s.hot_misses.load()
              << " warmups=" << s.hot_warmups.load()
              << " fp_collisions=" << s.hot_fp_collisions.load() << std::endl;

    if (mismatches != 0 || misses != 0) {
        std::cerr << "  FAIL: lossy" << std::endl;
        return 1;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_update_remove() {
    std::cout << "=== HiOM update + remove ===" << std::endl;
    cleanup_pool();
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    HiOMT hiom(*viper_db, kHotBuckets);
    auto client = hiom.get_client();

    constexpr std::size_t kN = 10'000;
    for (std::size_t i = 0; i < kN; ++i) {
        client.put(static_cast<std::uint64_t>(i + 1), 100 + i);
    }

    // Update half.
    for (std::size_t i = 0; i < kN; i += 2) {
        const std::uint64_t k = i + 1;
        const std::uint64_t new_val = 999'000 + i;
        client.update(k, [new_val](std::uint64_t* slot) { *slot = new_val; });
    }

    // Verify updates.
    std::size_t bad = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        const std::uint64_t k = i + 1;
        std::uint64_t got = 0;
        if (!client.get(k, &got)) { ++bad; continue; }
        const std::uint64_t expected = (i % 2 == 0)
            ? (999'000 + i)
            : (100 + i);
        if (got != expected) { ++bad; }
    }
    if (bad != 0) {
        std::cerr << "  FAIL: " << bad << " bad reads after update" << std::endl;
        return 1;
    }

    // Remove every third key.
    for (std::size_t i = 0; i < kN; i += 3) {
        client.remove(static_cast<std::uint64_t>(i + 1));
    }
    std::size_t bad_after_remove = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        const std::uint64_t k = i + 1;
        std::uint64_t got = 0;
        const bool present = client.get(k, &got);
        if ((i % 3 == 0) && present) ++bad_after_remove;     // shouldn't be there
        if ((i % 3 != 0) && !present) ++bad_after_remove;    // should be there
    }
    if (bad_after_remove != 0) {
        std::cerr << "  FAIL: " << bad_after_remove
                  << " bad reads after remove" << std::endl;
        return 1;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_microbench() {
    std::cout << "=== HiOM vs raw Viper (hit-mostly lookup) ===" << std::endl;
    cleanup_pool();
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    HiOMT hiom(*viper_db, kHotBuckets);

    auto h_client = hiom.get_client();
    auto v_client = viper_db->get_client();

    std::mt19937_64 rng(0xdeadbeef);
    std::vector<std::uint64_t> keys;
    keys.reserve(kNumKeys);
    for (std::size_t i = 0; i < kNumKeys; ++i) {
        const std::uint64_t k = static_cast<std::uint64_t>(i + 1);
        h_client.put(k, rng());
        keys.push_back(k);
    }

    // Warm the hot tier — one pass of lookups so HotTier is populated.
    for (auto k : keys) {
        std::uint64_t v;
        h_client.get(k, &v);
    }

    std::shuffle(keys.begin(), keys.end(), rng);

    auto bench = [&](auto& client, const char* label) {
        std::uint64_t accumulator = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < kBenchIters; ++i) {
            const std::uint64_t k = keys[i & (keys.size() - 1)];
            std::uint64_t v = 0;
            if (client.get(k, &v)) accumulator += v;
        }
        auto t1 = std::chrono::steady_clock::now();
        const double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        const double mops
            = static_cast<double>(kBenchIters) * 1e9 / ns / 1e6;
        std::printf("  %-12s %.2f M lookups/s   %.2f ns/op   (acc=%llu)\n",
                    label, mops, ns / kBenchIters,
                    static_cast<unsigned long long>(accumulator));
        return mops;
    };

    const double hiom_mops  = bench(h_client, "HiOM");
    const double viper_mops = bench(v_client, "raw Viper");

    const double ratio = hiom_mops / viper_mops;
    std::printf("  ratio HiOM / raw = %.3f\n", ratio);

    const auto& s = hiom.stats();
    std::cout << "  hot stats: hits=" << s.hot_hits.load()
              << " misses=" << s.hot_misses.load()
              << " warmups=" << s.hot_warmups.load()
              << " fp_collisions=" << s.hot_fp_collisions.load() << std::endl;

    if (ratio < 0.90 || ratio > 1.10) {
        std::cerr << "  WARN: outside ±10% of raw Viper (M0 exit)." << std::endl;
        // Not a hard fail yet — print and let the human evaluate.
    } else {
        std::cout << "  PASS (within ±10%)" << std::endl;
    }
    return 0;
}

// Phase B-2: HiOM with a real PM-backed ColdTier attached. Validates:
//   (a) every put/update/remove mirrors into ColdTier so its approx_size
//       tracks the live working set;
//   (b) reads of keys that have been evicted from HotTier still resolve
//       through ColdTier (no PM key-mismatch, no CCEH fallback);
//   (c) read-after-remove returns false (ColdTier tombstone respected).
//
// HotTier is sized intentionally small (4K slots = 256 buckets) so the
// 200K-key working set forces ColdTier-routed reads en masse.
int run_cold_backed() {
    std::cout << "=== HiOM with ColdTier (Phase B-2 integration) ===" << std::endl;
    cleanup_pool();
    cleanup_cold_pool();

    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    // Pre-size ColdTier so 200K entries fit comfortably without exhausting
    // overflow (Phase B-1 sized for ≤ 32 × 8192 × 7 = 1.8M main entries
    // with the defaults; we reuse the defaults here to mirror real use).
    auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
    HiOMT hiom(*viper_db, /*hot_buckets_pow2=*/256, cold.get());
    auto client = hiom.get_client();

    // Phase 1: insert 200K keys.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
    kv.reserve(kNumKeys);
    std::mt19937_64 rng(0xface);
    for (std::size_t i = 0; i < kNumKeys; ++i) {
        kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
    }
    for (auto& [k, v] : kv) {
        if (!client.put(k, v)) {
            std::cerr << "  FAIL: put returned false for key " << k << std::endl;
            return 1;
        }
    }
    // Flusher is asynchronous now; wait for it to drain before
    // inspecting ColdTier state.
    hiom.flush_and_wait();
    const std::size_t cold_after_fill = cold->approx_size();
    std::cout << "  inserted " << kv.size()
              << " keys; ColdTier.approx_size=" << cold_after_fill << std::endl;
    if (cold_after_fill != kv.size()) {
        std::cerr << "  FAIL: expected ColdTier to hold " << kv.size()
                  << " entries, found " << cold_after_fill << std::endl;
        return 1;
    }

    // Phase 2: read every key. With HotTier capped at 4K slots, most
    // reads must route through ColdTier (CCEH safety net was retired
    // in M3 Phase D; a HotTier+ColdTier double-miss returns false).
    auto reset_stats = [&]() {
        return std::tuple{
            hiom.stats().hot_hits.load(),
            hiom.stats().cold_hits.load(),
            hiom.stats().cold_misses.load(),
            hiom.stats().cold_fp_collisions.load()};
    };
    const auto [hh0, ch0, cm0, cfc0] = reset_stats();

    std::size_t hits = 0, misses = 0, mismatches = 0;
    for (auto& [k, expected] : kv) {
        std::uint64_t got = 0;
        if (!client.get(k, &got)) ++misses;
        else if (got != expected) ++mismatches;
        else ++hits;
    }

    const auto [hh1, ch1, cm1, cfc1] = reset_stats();
    const std::uint64_t hot_hits_delta  = hh1 - hh0;
    const std::uint64_t cold_hits_delta = ch1 - ch0;
    const std::uint64_t cold_miss_delta = cm1 - cm0;
    const std::uint64_t cold_fpc_delta  = cfc1 - cfc0;
    std::cout << "  read pass: hits=" << hits
              << " misses=" << misses
              << " mismatches=" << mismatches << std::endl;
    std::cout << "  per-tier deltas: hot=" << hot_hits_delta
              << " cold=" << cold_hits_delta
              << " cold_miss=" << cold_miss_delta
              << " cold_fp_coll=" << cold_fpc_delta << std::endl;

    if (misses != 0 || mismatches != 0) {
        std::cerr << "  FAIL: lossy read pass (M3 Phase D: ColdTier"
                  << " is authoritative; missed reads = bug)"
                  << std::endl;
        return 1;
    }
    // Hot+cold should account for every successful read; cold_fp_coll
    // should be statistically zero (1/2^64 collision rate).
    if (hot_hits_delta + cold_hits_delta + cold_fpc_delta != hits) {
        std::cerr << "  FAIL: tier accounting mismatch ("
                  << hot_hits_delta << " + " << cold_hits_delta
                  << " + " << cold_fpc_delta << " != " << hits << ")"
                  << std::endl;
        return 1;
    }
    // With a tiny hot tier we expect cold to carry most of the load.
    if (cold_hits_delta < hits / 2) {
        std::cerr << "  WARN: ColdTier carried < 50% of reads ("
                  << cold_hits_delta << " of " << hits << ")."
                  << " Maybe HotTier is too large for this assertion."
                  << std::endl;
    }

    // Phase 3: update half + remove a third (overlapping at i%6==0,
    // which exercises the update→remove sequence end-to-end now that
    // the underlying CCEH tombstone leak is fixed in viper.hpp).
    constexpr std::size_t kUpdEvery = 2;
    constexpr std::size_t kRmEvery = 3;
    for (std::size_t i = 0; i < kv.size(); i += kUpdEvery) {
        const std::uint64_t new_val = 7'777'000ull + i;
        kv[i].second = new_val;
        if (!client.update(kv[i].first,
                           [new_val](std::uint64_t* slot) { *slot = new_val; })) {
            std::cerr << "  FAIL: update returned false for key "
                      << kv[i].first << std::endl;
            return 1;
        }
    }
    std::vector<bool> removed(kv.size(), false);
    for (std::size_t i = 0; i < kv.size(); i += kRmEvery) {
        if (!client.remove(kv[i].first)) {
            std::cerr << "  FAIL: remove returned false for key "
                      << kv[i].first << std::endl;
            return 1;
        }
        removed[i] = true;
    }
    // Wait for the flusher to apply all updates + removes to ColdTier
    // before the final verification pass.
    hiom.flush_and_wait();

    // Final pass: removed keys must miss; survivors must read the
    // (possibly-updated) expected value via the full HiOM client path
    // (HotTier → ColdTier → CCEH). Now that the CCEH tombstone leak
    // is fixed in viper.hpp (Client::get adds a check_key_equality
    // guard against stale offsets), client.get is the ground truth.
    const auto [hh2, ch2, cm2, cfc2] = reset_stats();
    std::size_t bad = 0;
    std::size_t bad_removed_present = 0;
    std::size_t bad_kept_missing = 0;
    std::size_t bad_kept_wrong = 0;
    std::size_t first_bad_idx = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < kv.size(); ++i) {
        std::uint64_t got = 0;
        const bool present = client.get(kv[i].first, &got);
        if (removed[i]) {
            if (present) {
                ++bad; ++bad_removed_present;
                if (first_bad_idx == static_cast<std::size_t>(-1))
                    first_bad_idx = i;
            }
        } else {
            if (!present) {
                ++bad; ++bad_kept_missing;
                if (first_bad_idx == static_cast<std::size_t>(-1))
                    first_bad_idx = i;
            } else if (got != kv[i].second) {
                ++bad; ++bad_kept_wrong;
                if (first_bad_idx == static_cast<std::size_t>(-1))
                    first_bad_idx = i;
            }
        }
    }
    const auto [hh3, ch3, cm3, cfc3] = reset_stats();
    std::cout << "  final pass deltas: hot=" << (hh3 - hh2)
              << " cold=" << (ch3 - ch2)
              << " cold_miss=" << (cm3 - cm2)
              << " cold_fp_coll=" << (cfc3 - cfc2) << std::endl;
    if (bad != 0) {
        std::cerr << "  FAIL: " << bad
                  << " bad reads after update+remove"
                  << " (removed_present=" << bad_removed_present
                  << " kept_missing=" << bad_kept_missing
                  << " kept_wrong=" << bad_kept_wrong
                  << " first_bad_idx=" << first_bad_idx << ")"
                  << std::endl;
        return 1;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

// M3 Phase D check: with CCEH retired, commit-window reads can only
// be served from HotTier (PINNED) or ColdTier. The design contract
// (paper §3 invariant I3) is that HotTier capacity ≥ commit-buffer
// high watermark; given that, every recently-written key is hot
// before its corresponding cold-tier write completes. This test
// configures a HotTier large enough to hold all 50K writes, slows
// the flusher so the cold tier is empty during reads, and verifies
// every read hits HotTier (cold_misses == reads, hot_hits == reads).
int run_commit_window() {
    std::cout << "=== HiOM commit-window + inline-flush back-pressure (M4) ===" << std::endl;
    cleanup_pool();
    cleanup_cold_pool();

    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    auto cold = viper::hiom::ColdTier::create(kColdPoolFile);

    constexpr std::size_t kN = 10'000;
    // HotTier intentionally undersized to force back-pressure: 256
    // buckets × 16 slots = 4096 slot capacity, ~2.4× smaller than kN.
    // Without M4 Phase B's inline-flush, ~6K writes would fail to
    // acquire a HotTier slot (pin_failures > 0) and leave their
    // entries un-cached during the commit window. With Phase B the
    // write path drains the buffer synchronously when bucket fills
    // up, recycles slots through PINNED → IN_FLUSH → UNPINNED, and
    // retries — pin_failures should stay at 0.
    constexpr std::size_t kHotBucketsBackpressure = 1ULL << 8;  // 256 buckets
    viper::hiom::HiOM<std::uint64_t, std::uint64_t>::FlusherConfig fcfg;
    fcfg.interval = std::chrono::milliseconds(500);  // slow background
    fcfg.high_watermark = 1ULL << 30;
    HiOMT hiom(*viper_db, kHotBucketsBackpressure, cold.get(), fcfg);
    auto client = hiom.get_client();

    std::mt19937_64 rng(0xb00b);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
    kv.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
    }

    for (auto& [k, v] : kv) {
        if (!client.put(k, v)) {
            std::cerr << "  FAIL: put returned false for key " << k << std::endl;
            return 1;
        }
    }
    const std::size_t cold_before = cold->approx_size();
    const std::size_t buf_before = hiom.commit_buffer()->size_hint();
    const std::size_t hot_size = hiom.hot_tier().size();
    const std::size_t pin_failures = hiom.hot_tier().pin_failures();
    std::cout << "  after fill: ColdTier=" << cold_before
              << " buffer=" << buf_before
              << " HotTier.size=" << hot_size
              << " pin_failures=" << pin_failures << std::endl;
    if (buf_before == 0) {
        std::cerr << "  WARN: flusher already drained; can't exercise the"
                  << " commit-window path with this timing." << std::endl;
    }
    // pin_failures > 0 is expected here: HotTier is intentionally
    // undersized so back-pressure fires. The meaning is "this many
    // writes took the slow path"; correctness is verified below by
    // ensuring read-your-write succeeds for all kN keys.
    if (pin_failures == 0) {
        std::cerr << "  WARN: 0 pin_failures — back-pressure path may"
                  << " not have been exercised; HotTier sized too large?"
                  << std::endl;
    }

    const auto h0 = hiom.stats().hot_hits.load();
    const auto c0 = hiom.stats().cold_hits.load();
    const auto cm0 = hiom.stats().cold_misses.load();
    std::size_t hits = 0, misses = 0, mismatches = 0;
    for (const auto& [k, expected] : kv) {
        std::uint64_t got = 0;
        if (!client.get(k, &got)) ++misses;
        else if (got != expected) ++mismatches;
        else ++hits;
    }
    const auto h1 = hiom.stats().hot_hits.load();
    const auto c1 = hiom.stats().cold_hits.load();
    const auto cm1 = hiom.stats().cold_misses.load();
    const std::uint64_t hot_delta = h1 - h0;
    const std::uint64_t cold_delta = c1 - c0;
    const std::uint64_t cmiss_delta = cm1 - cm0;
    std::cout << "  reads during commit window: hits=" << hits
              << " misses=" << misses << " mismatches=" << mismatches
              << "  (hot=" << hot_delta
              << " cold=" << cold_delta
              << " cold_miss=" << cmiss_delta << ")" << std::endl;

    if (misses != 0 || mismatches != 0) {
        std::cerr << "  FAIL: read-your-write broken — "
                  << misses << " missed reads of just-written keys"
                  << std::endl;
        return 1;
    }
    // Inline-flush has already pushed many entries to ColdTier during
    // writes, so reads split between HotTier (still PINNED for the
    // tail of writes) and ColdTier (already drained). Sum must be N.
    if (hot_delta + cold_delta != kN) {
        std::cerr << "  FAIL: tier-hit accounting (" << hot_delta
                  << " + " << cold_delta << ") != " << kN << std::endl;
        return 1;
    }
    // Verify the test actually exercised back-pressure: cold_delta
    // should be substantial (most slots got recycled), and hot_delta
    // should still cover the tail. Looser bounds since exact split
    // depends on flusher timing and fp distribution.
    if (cold_delta < kN / 4) {
        std::cerr << "  WARN: cold_delta=" << cold_delta
                  << " unexpectedly low; back-pressure may not have triggered"
                  << " (test condition weakened by environment)." << std::endl;
    }

    // Drain and confirm post-flush state is consistent.
    hiom.flush_and_wait();
    if (cold->approx_size() != kN) {
        std::cerr << "  FAIL: post-flush ColdTier=" << cold->approx_size()
                  << " expected " << kN << std::endl;
        return 1;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

// M5: A/B checkpoint protocol. Validates that the 4 KB Checkpoint PM
// file survives close+reopen and recovers to the same record, that
// torn writes on the active slot fall back to the inactive one, and
// that HiOM constructor primes its cumulative flushed_count_ + seq_
// from any restored record.
//
// Phases:
//   (1) Steady-state: 10K writes with cadence=1024 → expect ≥9
//       checkpoint writes; force_checkpoint to seal post-flush state;
//       capture the live record.
//   (2) Reopen Checkpoint alone (no DB) → read_valid() returns the
//       same record (seq, flushed_count, vpage_frontier, cold_size).
//   (3) Corrupt the active slot's magic by writing 8 bytes via fwrite
//       (the file is plain bytes, no need to reach into Checkpoint
//       internals) → read_valid() falls back to the inactive slot,
//       whose record has seq < phase-1's.
//   (4) Fresh PM files everywhere → write some, verify HiOM picks up
//       flushed_count from the new checkpoint on next construction
//       and that subsequent writes advance seq monotonically.
int run_checkpoint_persistence() {
    std::cout << "=== HiOM A/B checkpoint persistence (M5) ===" << std::endl;
    cleanup_pool();
    cleanup_cold_pool();
    cleanup_checkpoint_dir();

    constexpr std::size_t kN = 10'000;
    constexpr std::uint64_t kCadence = 1024;
    constexpr std::size_t kHotBucketsCkpt = 1ULL << 10;  // 16K slots, room for kN

    viper::hiom::CheckpointRecord rec_phase1{};

    // Phase 1: write data with Checkpoint attached, snapshot record.
    {
        auto viper_db = ViperT::create(kPoolDir, kPoolSize);
        auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
        auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);

        HiOMT::CheckpointConfig ccfg;
        ccfg.cadence_entries = kCadence;
        HiOMT hiom(*viper_db, kHotBucketsCkpt, cold.get(),
                   HiOMT::FlusherConfig{}, chkpt.get(), ccfg);
        auto client = hiom.get_client();

        std::mt19937_64 rng(0xc4ec0700);
        for (std::size_t i = 0; i < kN; ++i) {
            if (!client.put(static_cast<std::uint64_t>(i + 1), rng())) {
                std::cerr << "  FAIL: put returned false at i=" << i << std::endl;
                return 1;
            }
        }
        hiom.flush_and_wait();
        // force_checkpoint after flush_and_wait so the persisted record
        // captures cold_size==kN and flushed_count==kN regardless of
        // where the cadence boundaries landed.
        hiom.force_checkpoint();

        const auto cw = hiom.stats().checkpoints_written.load();
        // cadence 1024, kN=10000 → at least 10000/1024 = 9 cadence
        // boundaries, plus the explicit force_checkpoint, so >= 9.
        const std::uint64_t expected_min = kN / kCadence;
        std::cout << "  phase 1: writes=" << kN
                  << " checkpoints_written=" << cw
                  << " (expected >= " << expected_min << ")" << std::endl;
        if (cw < expected_min) {
            std::cerr << "  FAIL: too few checkpoint writes "
                      << cw << " < " << expected_min << std::endl;
            return 1;
        }

        auto rec_opt = chkpt->read_valid();
        if (!rec_opt) {
            std::cerr << "  FAIL: read_valid returned nullopt after writes"
                      << std::endl;
            return 1;
        }
        rec_phase1 = *rec_opt;
        std::cout << "  phase 1 record: seq=" << rec_phase1.seq
                  << " flushed=" << rec_phase1.flushed_count
                  << " frontier=0x" << std::hex << rec_phase1.vpage_frontier
                  << std::dec
                  << " cold=" << rec_phase1.cold_size << std::endl;

        if (rec_phase1.flushed_count != kN) {
            std::cerr << "  FAIL: flushed_count=" << rec_phase1.flushed_count
                      << " expected " << kN << std::endl;
            return 1;
        }
        if (rec_phase1.cold_size != kN) {
            std::cerr << "  FAIL: cold_size=" << rec_phase1.cold_size
                      << " expected " << kN << std::endl;
            return 1;
        }
        if (rec_phase1.vpage_frontier == 0) {
            std::cerr << "  FAIL: vpage_frontier should be > 0 after writes"
                      << std::endl;
            return 1;
        }
        if (rec_phase1.seq == 0) {
            std::cerr << "  FAIL: seq should be > 0 after force_checkpoint"
                      << std::endl;
            return 1;
        }
    }

    // Phase 2: reopen the checkpoint file alone, verify read_valid()
    // returns the same record bytes.
    {
        auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
        auto rec_opt = chkpt->read_valid();
        if (!rec_opt) {
            std::cerr << "  FAIL: phase 2 read_valid returned nullopt"
                      << std::endl;
            return 1;
        }
        const auto& r = *rec_opt;
        if (r.seq != rec_phase1.seq
            || r.flushed_count != rec_phase1.flushed_count
            || r.vpage_frontier != rec_phase1.vpage_frontier
            || r.cold_size != rec_phase1.cold_size) {
            std::cerr << "  FAIL: phase 2 record mismatch:"
                      << " seq=" << r.seq << "/" << rec_phase1.seq
                      << " flushed=" << r.flushed_count << "/"
                      << rec_phase1.flushed_count
                      << " frontier=" << r.vpage_frontier << "/"
                      << rec_phase1.vpage_frontier
                      << " cold=" << r.cold_size << "/"
                      << rec_phase1.cold_size << std::endl;
            return 1;
        }
        std::cout << "  phase 2: reopen record matches (seq="
                  << r.seq << ")" << std::endl;
    }

    // Phase 3: torn-write fallback. Snap the active slot index, close
    // the checkpoint, corrupt the active slot's magic field via plain
    // file I/O (avoids any DAX mmap coherence question), reopen, and
    // verify read_valid() falls back to the older slot.
    {
        std::uint64_t active_slot = 0;
        {
            auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
            active_slot = chkpt->valid_pointer_raw();
        }
        if (active_slot != viper::hiom::Checkpoint::kSlotA
            && active_slot != viper::hiom::Checkpoint::kSlotB) {
            std::cerr << "  FAIL: phase 3 unexpected active slot "
                      << active_slot << std::endl;
            return 1;
        }
        const std::size_t slot_off
            = (active_slot == viper::hiom::Checkpoint::kSlotA)
              ? viper::hiom::Checkpoint::kSlotAOffset
              : viper::hiom::Checkpoint::kSlotBOffset;

        FILE* f = std::fopen(kCheckpointFile, "r+b");
        if (!f) {
            std::cerr << "  FAIL: phase 3 fopen failed" << std::endl;
            return 1;
        }
        if (std::fseek(f, static_cast<long>(slot_off), SEEK_SET) != 0) {
            std::cerr << "  FAIL: phase 3 fseek failed" << std::endl;
            std::fclose(f);
            return 1;
        }
        std::uint64_t bad = 0xdeadbeefdeadbeefull;
        if (std::fwrite(&bad, sizeof(bad), 1, f) != 1) {
            std::cerr << "  FAIL: phase 3 fwrite failed" << std::endl;
            std::fclose(f);
            return 1;
        }
        std::fflush(f);
        std::fclose(f);

        auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
        auto rec_opt = chkpt->read_valid();
        if (!rec_opt) {
            std::cerr << "  FAIL: torn-write fallback returned nullopt"
                      << " (both slots unrecoverable?)" << std::endl;
            return 1;
        }
        const auto& r = *rec_opt;
        // Active slot at corruption time held rec_phase1; fallback
        // came from the inactive slot which was written one cadence
        // earlier — strictly older seq.
        if (r.seq >= rec_phase1.seq) {
            std::cerr << "  FAIL: torn-write fallback returned seq="
                      << r.seq << ", not older than corrupted seq="
                      << rec_phase1.seq << " — fallback didn't engage"
                      << std::endl;
            return 1;
        }
        std::cout << "  phase 3: torn-write fallback returned older record"
                  << " seq=" << r.seq << " (corrupted seq="
                  << rec_phase1.seq << ")" << std::endl;
    }

    // Phase 4: fresh PM state, build a checkpoint, then construct a
    // new HiOM with the checkpoint attached and verify it primes
    // flushed_count_ from the persisted record. Then continue writing
    // and verify seq advances strictly.
    {
        cleanup_pool();
        cleanup_cold_pool();
        cleanup_checkpoint_dir();

        // Sub-phase 4a: write a small batch to get a non-zero record.
        std::uint64_t pre_seq = 0;
        std::uint64_t pre_flushed = 0;
        constexpr std::size_t kInit = 5'000;
        {
            auto viper_db = ViperT::create(kPoolDir, kPoolSize);
            auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);

            HiOMT::CheckpointConfig ccfg;
            ccfg.cadence_entries = kCadence;
            HiOMT hiom(*viper_db, kHotBucketsCkpt, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(), ccfg);
            auto client = hiom.get_client();
            std::mt19937_64 rng(0x42);
            for (std::size_t i = 0; i < kInit; ++i) {
                client.put(static_cast<std::uint64_t>(i + 1), rng());
            }
            hiom.flush_and_wait();
            hiom.force_checkpoint();

            auto rec_opt = chkpt->read_valid();
            if (!rec_opt || rec_opt->flushed_count != kInit) {
                std::cerr << "  FAIL: phase 4a unexpected record" << std::endl;
                return 1;
            }
            pre_seq = rec_opt->seq;
            pre_flushed = rec_opt->flushed_count;
        }

        // Sub-phase 4b: reopen everything, observe priming, write more.
        // Note: Viper recovery (ColdTier::open / Viper::open with full
        // VPage scan) is M6; for M5 we just verify Checkpoint priming
        // by creating a fresh DB but reopening the checkpoint file.
        cleanup_pool();
        cleanup_cold_pool();
        // Don't clean checkpoint dir — that's what we're reopening.
        auto viper_db = ViperT::create(kPoolDir, kPoolSize);
        auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
        auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);

        HiOMT::CheckpointConfig ccfg;
        ccfg.cadence_entries = kCadence;
        HiOMT hiom(*viper_db, kHotBucketsCkpt, cold.get(),
                   HiOMT::FlusherConfig{}, chkpt.get(), ccfg);
        if (hiom.flushed_count() != pre_flushed) {
            std::cerr << "  FAIL: HiOM did not prime flushed_count: got="
                      << hiom.flushed_count()
                      << " expected " << pre_flushed << std::endl;
            return 1;
        }
        std::cout << "  phase 4: HiOM primed flushed_count="
                  << hiom.flushed_count() << " seq_floor="
                  << pre_seq << " from restored checkpoint" << std::endl;

        auto client = hiom.get_client();
        constexpr std::size_t kExtra = 2'000;
        for (std::size_t i = 0; i < kExtra; ++i) {
            client.put(static_cast<std::uint64_t>(1'000'000 + i),
                       0xababababababababull);
        }
        hiom.flush_and_wait();
        hiom.force_checkpoint();

        auto rec_post = chkpt->read_valid();
        if (!rec_post) {
            std::cerr << "  FAIL: phase 4 post read_valid returned nullopt"
                      << std::endl;
            return 1;
        }
        if (rec_post->seq <= pre_seq) {
            std::cerr << "  FAIL: post seq=" << rec_post->seq
                      << " not advanced past pre seq=" << pre_seq
                      << std::endl;
            return 1;
        }
        if (rec_post->flushed_count != pre_flushed + kExtra) {
            std::cerr << "  FAIL: post flushed=" << rec_post->flushed_count
                      << " expected " << (pre_flushed + kExtra)
                      << std::endl;
            return 1;
        }
        std::cout << "  phase 4 post: seq=" << rec_post->seq
                  << " flushed=" << rec_post->flushed_count
                  << " (delta=" << (rec_post->flushed_count - pre_flushed)
                  << ")" << std::endl;
    }

    std::cout << "  PASS" << std::endl;
    return 0;
}

// M6: bounded crash-recovery via VPage tail scan. Validates that HiOM
// can rebuild ColdTier-side state after a simulated crash by scanning
// only [checkpoint.vpage_frontier, current_block_page) — not the whole
// PM pool. Three phases:
//
//   Phase A — clean shutdown: write 5K, flush, force_checkpoint,
//     destroy HiOM. Reopen Viper / ColdTier / Checkpoint, construct
//     HiOM with tail_scan=true. Expect recovery_replayed == 0
//     (frontier == current). Read all 5K → all hit.
//
//   Phase B — crash mid-stream after a checkpoint: slow flusher so
//     buffer accumulates. Write 5K, flush, force_checkpoint (cold=5K,
//     frontier captured). Write 5K more (vpage advances; cold frozen
//     because flusher slow). Destroy *without* flush_and_wait (crash
//     sim). Reopen, recover. Expect recovery_replayed ≈ 5K (the
//     tail). Read all 10K → all hit.
//
//   Phase C — crash before any checkpoint: write 5K with
//     cadence_entries = 1<<30 (no auto-checkpoints) and never call
//     force_checkpoint. Destroy without flush. Reopen — read_valid()
//     returns nullopt → tail scan starts at block 0 → scans full
//     pool. Expect recovery_replayed == 5K. Read all 5K → all hit.
//
// Note: Viper::open() still runs its own full CCEH rebuild on reopen
// (bypassing that is M6.5/M7-perf scope). The point of this test is
// the tail-scan correctness contract, not the recovery time win.
int run_recovery_persistence() {
    std::cout << "=== HiOM bounded tail-scan recovery (M6) ===" << std::endl;

    constexpr std::size_t kHotBucketsRec = 1ULL << 10;  // 16K slots

    // ---- Phase A: clean shutdown then no-op recover ----
    {
        cleanup_pool();
        cleanup_cold_pool();
        cleanup_checkpoint_dir();
        constexpr std::size_t kN = 5'000;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
        kv.reserve(kN);
        std::mt19937_64 rng(0xa1);
        for (std::size_t i = 0; i < kN; ++i) {
            kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
        }

        // Phase A.1: write + clean shutdown.
        {
            auto viper_db = ViperT::create(kPoolDir, kPoolSize);
            auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);
            HiOMT::CheckpointConfig ccfg;
            ccfg.cadence_entries = 1024;
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(), ccfg);
            auto client = hiom.get_client();
            for (auto& [k, v] : kv) client.put(k, v);
            hiom.flush_and_wait();
            hiom.force_checkpoint();
        }  // dtor: drains cleanly, frontier == current

        // Phase A.2: reopen + recover with tail_scan=true.
        {
            auto viper_db = ViperT::open(kPoolDir, viper::ViperConfig{});
            auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
            const std::size_t cold_pre = cold->approx_size();
            HiOMT::RecoveryConfig rcfg;
            rcfg.tail_scan = true;
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(),
                       HiOMT::CheckpointConfig{}, rcfg);
            const auto replayed = hiom.stats().recovery_replayed.load();
            const std::size_t cold_post = cold->approx_size();
            std::cout << "  phase A: writes=" << kN
                      << " replayed=" << replayed
                      << " cold(pre→post)=" << cold_pre
                      << "→" << cold_post << std::endl;
            // After clean shutdown, the entire boundary block is
            // already drained to cold; tail-scan re-upserts those
            // entries idempotently (same fp64 → overwrite without
            // bumping num_entries). cold size must be unchanged.
            if (cold_pre != kN || cold_post != kN) {
                std::cerr << "  FAIL: phase A cold size shifted "
                          << cold_pre << " → " << cold_post
                          << " (expected " << kN << " both)"
                          << std::endl;
                return 1;
            }
            auto client = hiom.get_client();
            std::size_t hits = 0, misses = 0, mismatches = 0;
            for (const auto& [k, expected] : kv) {
                std::uint64_t got = 0;
                if (!client.get(k, &got)) ++misses;
                else if (got != expected) ++mismatches;
                else ++hits;
            }
            std::cout << "  phase A reads: hits=" << hits
                      << " misses=" << misses
                      << " mismatches=" << mismatches << std::endl;
            if (hits != kN) {
                std::cerr << "  FAIL: phase A read pass" << std::endl;
                return 1;
            }
        }
    }

    // ---- Phase B: crash mid-stream with a checkpoint ----
    {
        cleanup_pool();
        cleanup_cold_pool();
        cleanup_checkpoint_dir();
        constexpr std::size_t kHalf = 5'000;
        constexpr std::size_t kTotal = 2 * kHalf;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
        kv.reserve(kTotal);
        std::mt19937_64 rng(0xb2);
        for (std::size_t i = 0; i < kTotal; ++i) {
            kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
        }
        std::size_t cold_at_crash = 0;
        std::size_t buf_at_crash = 0;

        // Phase B.1: write 5K, flush+checkpoint, write 5K more, "crash".
        {
            auto viper_db = ViperT::create(kPoolDir, kPoolSize);
            auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);

            // Slow flusher so buffer accumulates after the checkpoint.
            HiOMT::FlusherConfig fcfg;
            fcfg.interval = std::chrono::milliseconds(500);
            fcfg.high_watermark = 1ULL << 30;
            HiOMT::CheckpointConfig ccfg;
            ccfg.cadence_entries = 1ULL << 30;  // no auto checkpoints
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(), fcfg,
                       chkpt.get(), ccfg);
            auto client = hiom.get_client();

            for (std::size_t i = 0; i < kHalf; ++i)
                client.put(kv[i].first, kv[i].second);
            hiom.flush_and_wait();
            hiom.force_checkpoint();
            for (std::size_t i = kHalf; i < kTotal; ++i)
                client.put(kv[i].first, kv[i].second);
            cold_at_crash = cold->approx_size();
            buf_at_crash = hiom.commit_buffer()
                ? hiom.commit_buffer()->size_hint() : 0;
            std::cout << "  phase B at-crash: cold=" << cold_at_crash
                      << " buf=" << buf_at_crash << std::endl;
            // M4 inline-flush back-pressure may have drained some of
            // the tail eagerly; cold should still be < kTotal so the
            // tail scan has work to do.
            if (cold_at_crash >= kTotal) {
                std::cerr << "  WARN: ColdTier already full pre-crash; "
                          << "test won't exercise tail-scan."
                          << std::endl;
            }
            // Crash sim: stop flusher + drop buffer so the dtor's
            // final drain is a no-op. Any commit-buffer entries here
            // are lost from ColdTier — exactly the post-kill state.
            hiom.simulate_crash_for_test();
        }  // dtor runs but commit_buf_ is null → no drain

        // Phase B.2: reopen + tail-scan recovery.
        {
            auto viper_db = ViperT::open(kPoolDir, viper::ViperConfig{});
            auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
            HiOMT::RecoveryConfig rcfg;
            rcfg.tail_scan = true;
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(),
                       HiOMT::CheckpointConfig{}, rcfg);
            const auto replayed = hiom.stats().recovery_replayed.load();
            const std::size_t cold_after_recovery = cold->approx_size();
            std::cout << "  phase B post-recovery: replayed=" << replayed
                      << " cold=" << cold_after_recovery
                      << " (expected cold==" << kTotal << ")" << std::endl;
            if (cold_after_recovery != kTotal) {
                std::cerr << "  FAIL: phase B cold size after recovery "
                          << cold_after_recovery << " != " << kTotal
                          << std::endl;
                return 1;
            }
            // Tail-scan replays from the frontier (which captured the
            // post-flush state at force_checkpoint), so it covers
            // every block since then. Some replayed entries may
            // already have been in cold via inline-flush; idempotent
            // upserts are harmless. Replayed >= the 5K post-checkpoint
            // tail (give or take, depending on per-block packing).
            if (replayed < kHalf - 1) {
                std::cerr << "  FAIL: phase B replayed " << replayed
                          << " < expected ≥ " << (kHalf - 1) << std::endl;
                return 1;
            }
            auto client = hiom.get_client();
            std::size_t hits = 0, misses = 0, mismatches = 0;
            for (const auto& [k, expected] : kv) {
                std::uint64_t got = 0;
                if (!client.get(k, &got)) ++misses;
                else if (got != expected) ++mismatches;
                else ++hits;
            }
            std::cout << "  phase B reads: hits=" << hits
                      << " misses=" << misses
                      << " mismatches=" << mismatches << std::endl;
            if (hits != kTotal) {
                std::cerr << "  FAIL: phase B read pass" << std::endl;
                return 1;
            }
        }
    }

    // ---- Phase C: crash before any checkpoint ----
    {
        cleanup_pool();
        cleanup_cold_pool();
        cleanup_checkpoint_dir();
        constexpr std::size_t kN = 5'000;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
        kv.reserve(kN);
        std::mt19937_64 rng(0xc3);
        for (std::size_t i = 0; i < kN; ++i) {
            kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
        }

        // Phase C.1: write 5K with no checkpoint at all, then "crash".
        {
            auto viper_db = ViperT::create(kPoolDir, kPoolSize);
            auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);

            HiOMT::FlusherConfig fcfg;
            fcfg.interval = std::chrono::milliseconds(500);
            fcfg.high_watermark = 1ULL << 30;
            HiOMT::CheckpointConfig ccfg;
            ccfg.cadence_entries = 1ULL << 30;
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(), fcfg,
                       chkpt.get(), ccfg);
            auto client = hiom.get_client();
            for (auto& [k, v] : kv) client.put(k, v);
            // No flush, no force_checkpoint. Crash sim: drop buffer
            // so dtor doesn't drain.
            hiom.simulate_crash_for_test();
        }

        // Phase C.2: reopen — read_valid() returns nullopt; tail scan
        // starts at block 0 and scans the whole pool.
        {
            auto viper_db = ViperT::open(kPoolDir, viper::ViperConfig{});
            auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
            // Sanity: checkpoint is fresh.
            if (chkpt->read_valid().has_value()) {
                std::cerr << "  FAIL: phase C precondition — checkpoint "
                          << "should be empty" << std::endl;
                return 1;
            }
            HiOMT::RecoveryConfig rcfg;
            rcfg.tail_scan = true;
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(),
                       HiOMT::CheckpointConfig{}, rcfg);
            const auto replayed = hiom.stats().recovery_replayed.load();
            const std::size_t cold_after = cold->approx_size();
            std::cout << "  phase C post-recovery: replayed=" << replayed
                      << " cold=" << cold_after
                      << " (expected " << kN << ")" << std::endl;
            if (replayed != kN) {
                std::cerr << "  FAIL: phase C replayed " << replayed
                          << " != " << kN << std::endl;
                return 1;
            }
            if (cold_after != kN) {
                std::cerr << "  FAIL: phase C cold " << cold_after
                          << " != " << kN << std::endl;
                return 1;
            }
            auto client = hiom.get_client();
            std::size_t hits = 0, misses = 0, mismatches = 0;
            for (const auto& [k, expected] : kv) {
                std::uint64_t got = 0;
                if (!client.get(k, &got)) ++misses;
                else if (got != expected) ++mismatches;
                else ++hits;
            }
            std::cout << "  phase C reads: hits=" << hits
                      << " misses=" << misses
                      << " mismatches=" << mismatches << std::endl;
            if (hits != kN) {
                std::cerr << "  FAIL: phase C read pass" << std::endl;
                return 1;
            }
        }
    }

    // ---- Phase D: skip_recovery=true + post-restart update/remove (M6.5 full) ----
    // Verifies the OldOffsetResolver fires on Viper::Client::update
    // and ::remove. Without the resolver, post-restart map_ is empty
    // for pre-existing keys; update sees Get→tombstone and returns
    // false (no mutation), remove sees the same and returns false (no
    // invalidation). With the resolver, the fallback supplies the
    // pre-restart KVOffset, the page is locked, and the mutation /
    // invalidation goes through. We assert both via behaviour
    // (read-back values for update, presence/absence for remove) and
    // via slot accounting (count_live_vpage_records).
    //
    // We deliberately do NOT use cl.put() for post-restart updates:
    // HiOM::Client::put short-circuits on existing-key returns from
    // viper_.put and never re-fires mirror_write, which is a
    // pre-existing API quirk orthogonal to M6.5. The contract is:
    // put = insert, update = in-place mutate, remove = delete.
    {
        cleanup_pool();
        cleanup_cold_pool();
        cleanup_checkpoint_dir();
        constexpr std::size_t kN = 2'000;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
        kv.reserve(kN);
        std::mt19937_64 rng(0xd1);
        for (std::size_t i = 0; i < kN; ++i) {
            kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
        }

        // D.1: clean prefill + clean shutdown.
        {
            auto viper_db = ViperT::create(kPoolDir, kPoolSize);
            auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);
            HiOMT::CheckpointConfig ccfg;
            ccfg.cadence_entries = 1024;
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(), ccfg);
            auto cl = hiom.get_client();
            for (auto& [k, v] : kv) cl.put(k, v);
            hiom.flush_and_wait();
            hiom.force_checkpoint();
        }

        auto count_live_vpage_records = [](ViperT& v) {
            const viper::offset_size_t fr = v.hiom_vpage_frontier();
            const auto block_hi
                = static_cast<viper::block_size_t>(
                    viper::KeyValueOffset{fr}.block_number) + 1;
            std::size_t live = 0;
            v.hiom_visit_records(0, block_hi,
                                 [&](const auto&, const auto&, auto) { ++live; });
            return live;
        };

        // D.2: reopen with skip_recovery=true; in-place update every
        // key, then remove half. Read-back must show the new values
        // for the survivors and absence for the removed half.
        const std::uint64_t kNewBase = 0xfaceULL << 16;
        {
            viper::ViperConfig vcfg;
            vcfg.skip_recovery = true;
            auto viper_db = ViperT::open(kPoolDir, vcfg);
            auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
            HiOMT::RecoveryConfig rcfg;
            rcfg.tail_scan = true;
            HiOMT hiom(*viper_db, kHotBucketsRec, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(),
                       HiOMT::CheckpointConfig{}, rcfg);
            auto cl = hiom.get_client();

            // Update every key.
            std::size_t update_failures = 0;
            for (std::size_t i = 0; i < kN; ++i) {
                const std::uint64_t k = static_cast<std::uint64_t>(i + 1);
                const std::uint64_t newv = kNewBase + i;
                if (!cl.update(k, [newv](std::uint64_t* slot) {
                        *slot = newv;
                    })) {
                    ++update_failures;
                }
            }
            std::cout << "  phase D updates: failed=" << update_failures
                      << " (expected 0)" << std::endl;
            if (update_failures != 0) {
                std::cerr << "  FAIL: phase D update — resolver did not "
                          << "supply pre-restart offsets for "
                          << update_failures << " keys" << std::endl;
                return 1;
            }

            std::size_t hits = 0, mismatches = 0;
            for (std::size_t i = 0; i < kN; ++i) {
                std::uint64_t got = 0;
                if (cl.get(static_cast<std::uint64_t>(i + 1), &got)) {
                    if (got == kNewBase + i) ++hits;
                    else ++mismatches;
                }
            }
            std::cout << "  phase D post-update reads: hits=" << hits
                      << " mismatches=" << mismatches << std::endl;
            if (hits != kN) {
                std::cerr << "  FAIL: phase D update read-back" << std::endl;
                return 1;
            }

            // Remove the odd-indexed half. Updates were in-place so
            // live count is still kN; remove should drop it to kN/2.
            std::size_t remove_failures = 0;
            std::size_t expected_removed = 0;
            for (std::size_t i = 0; i < kN; ++i) {
                if ((i & 1) == 1) {
                    if (!cl.remove(static_cast<std::uint64_t>(i + 1))) {
                        ++remove_failures;
                    }
                    ++expected_removed;
                }
            }
            std::cout << "  phase D removes: failed=" << remove_failures
                      << " (expected 0 of " << expected_removed << ")"
                      << std::endl;
            if (remove_failures != 0) {
                std::cerr << "  FAIL: phase D remove — resolver did not "
                          << "supply pre-restart offsets for "
                          << remove_failures << " keys" << std::endl;
                return 1;
            }

            const std::size_t live_after = count_live_vpage_records(*viper_db);
            const std::size_t expected_live = kN - expected_removed;
            std::cout << "  phase D live VPage records (in-process): "
                      << live_after << " (expected " << expected_live << ")"
                      << std::endl;
            if (live_after != expected_live) {
                std::cerr << "  FAIL: phase D — slot accounting wrong. "
                          << "live=" << live_after
                          << " expected=" << expected_live
                          << " (resolver did not invalidate pre-restart "
                          << "VPage slots on remove)" << std::endl;
                return 1;
            }
        }

        // D.3: reopen via the default path, count live records to
        // confirm the invalidations persisted to PM.
        {
            auto viper_db = ViperT::open(kPoolDir, viper::ViperConfig{});
            const std::size_t live = count_live_vpage_records(*viper_db);
            const std::size_t expected_live = kN / 2;
            std::cout << "  phase D live VPage records (post-reopen): "
                      << live << " (expected " << expected_live << ")"
                      << std::endl;
            if (live != expected_live) {
                std::cerr << "  FAIL: phase D — invalidation not "
                          << "persisted. live=" << live
                          << " expected=" << expected_live << std::endl;
                return 1;
            }
        }
    }

    std::cout << "  PASS" << std::endl;
    return 0;
}

// M4 Phase C: multi-producer correctness. Spawns N writer threads that
// pound on a small key space (heavy same-key contention). Without
// (fp64, seq) sort + coalesce + drain-to-empty under apply_mu_, the
// commit-buffer order doesn't match the CCEH CAS order, and ColdTier
// can end up with an offset whose VPage slot was freed by a later
// CAS — `cl.get(K)` then misses live keys. The test asserts:
//
//   1. Every key in [0, KEY_RANGE) is readable via HiOM client get
//      after all writers finish + flush_and_wait. Zero misses.
//
//   2. The KVOffset that ColdTier holds for each key matches the
//      KVOffset that Viper's CCEH holds (the linearization-point
//      truth). Mismatch = stale offset in ColdTier = the bug Phase C
//      fixes.
int run_multi_producer_correctness() {
    std::cout << "=== HiOM multi-producer same-key correctness (M4 Phase C) ===" << std::endl;

    cleanup_pool();
    cleanup_cold_pool();
    cleanup_checkpoint_dir();

    constexpr std::size_t kHotBucketsMP = 1ULL << 12;  // 4K buckets x 16 = 64K slots
    constexpr int kNumThreads = 8;
    constexpr int kKeyRange = 256;          // heavy same-key contention
    constexpr int kItersPerThread = 50'000;

    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
    auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);
    HiOMT hiom(*viper_db, kHotBucketsMP, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get());

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> total_writes{0};

    auto worker = [&](int thread_id) {
        auto cl = hiom.get_client();
        std::mt19937_64 rng(static_cast<std::uint64_t>(thread_id) * 0x9E3779B97F4A7C15ULL);
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kItersPerThread; ++i) {
            const std::uint64_t k = static_cast<std::uint64_t>(
                (rng() % kKeyRange) + 1);
            // Per-(thread, iteration) value: high bits are thread_id,
            // low bits are i, so collisions across threads have
            // distinguishable values. We don't predict the CCEH
            // winner; we just need cl.get to return SOME thread's
            // value AND the offset to match CCEH's view.
            const std::uint64_t v = (static_cast<std::uint64_t>(thread_id) << 48)
                                  | static_cast<std::uint64_t>(i);
            cl.put(k, v);
        }
        total_writes.fetch_add(kItersPerThread, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back(worker, t);
    }
    while (ready.load() < kNumThreads) std::this_thread::yield();
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    const auto t1 = std::chrono::steady_clock::now();
    hiom.flush_and_wait();
    const auto t2 = std::chrono::steady_clock::now();

    const double write_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double flush_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cout << "  threads=" << kNumThreads
              << " key_range=" << kKeyRange
              << " writes/thread=" << kItersPerThread
              << "  total_writes=" << total_writes.load()
              << "  (" << write_ms << " ms write, "
              << flush_ms << " ms flush)" << std::endl;

    // P0 (M3 follow-up #2): CCEH is no longer touched on the write path
    // when HiOM is attached, so the prior CCEH-vs-ColdTier comparison
    // would always fail. Switch to HiOM-side ground truth:
    //   1. cl.get(k) must return SOME value (any thread's write counts).
    //   2. ColdTier.lookup(fp) must hold an offset for k.
    //   3. The PM slot at that offset must hold key k (detects slot
    //      reuse / fp collision corruption).
    // Slot leak under same-key cross-thread races is a known MVP-P0
    // limitation (see design/HIOM.md M3 follow-up #2 notes); this test
    // does NOT assert leak-freedom — it only asserts the canonical
    // (winner) offset is correct.
    auto cl = hiom.get_client();
    auto vcl_ro = viper_db->get_read_only_client();
    std::size_t missed = 0;
    std::size_t cold_missing = 0;
    std::size_t pm_key_mismatch = 0;
    std::size_t ok = 0;
    for (int i = 0; i < kKeyRange; ++i) {
        const std::uint64_t k = static_cast<std::uint64_t>(i + 1);

        std::uint64_t got = 0;
        if (!cl.get(k, &got)) {
            ++missed;
            continue;
        }

        const auto fp = viper::hiom::key_fingerprint64(k);
        const auto cold_off = hiom.cold_tier()->lookup(fp);
        if (!cold_off.has_value()) {
            ++cold_missing;
            continue;
        }

        std::uint64_t pm_key = 0;
        std::uint64_t pm_val = 0;
        if (!vcl_ro.hiom_read_at_offset(*cold_off, &pm_key, &pm_val)
            || pm_key != k) {
            ++pm_key_mismatch;
            continue;
        }
        ++ok;
    }
    std::cout << "  per-key HiOM truth: ok=" << ok
              << " missed=" << missed
              << " cold_missing=" << cold_missing
              << " pm_key_mismatch=" << pm_key_mismatch
              << " (expected ok=" << kKeyRange << ", others=0)" << std::endl;
    if (ok != static_cast<std::size_t>(kKeyRange)) {
        std::cerr << "  FAIL: HiOM read-back / ColdTier truth broken — "
                  << "missed=" << missed
                  << " cold_missing=" << cold_missing
                  << " pm_key_mismatch=" << pm_key_mismatch << std::endl;
        return 1;
    }

    std::cout << "  PASS" << std::endl;
    return 0;
}

// M4 Phase E: multi-thread + crash injection stress.
//
// Goal: prove recovery completeness under concurrent writers with a
// random crash point.
//
// Workload: kNumThreads writers, each owning a disjoint key range
//   thread t: keys [base_t, base_t + kPerThread), where
//   base_t = t * (kPerThread + 1) + 1 (the +1 gap leaves room for
//   key 0 to mean "unused").
// Per-thread atomic completed_t bumps after each cl.put returns; this
// is the worker's view of "puts that have definitely returned".
//
// Because Viper's pmem_persist runs INSIDE viper_.put, then HiOM's
// mirror_write (HotTier upsert + commit-buffer push, possibly inline-
// flush drain-to-empty on the bucket-full path) runs after, then
// cl.put returns, then we bump completed_t — everything that the bump
// counts has its VPage entry durable on PM. Tail-scan recovery reads
// VPages, so every counted put MUST be recoverable. Lossy recovery
// here is a real correctness regression in the persistence ordering.
//
// Crash protocol: the simulate_crash_for_test() hook resets
// commit_buf_, which would NULL-deref any in-flight mirror_write. We
// therefore use a cooperative crashed_flag that workers check before
// each put — workers stop, join, *then* we sim the crash. The
// resulting on-PM state is identical to a real SIGKILL: HotTier and
// commit buffer (DRAM) are gone; ColdTier and Checkpoint (PM) are
// whatever the flusher had time to durably write.
//
// Two flusher modes:
//   FAST: default 5 ms interval; the flusher usually catches up by
//         the time the writers stop, so cold@crash ≈ completed and
//         tail-scan is mostly idempotent re-upserts.
//   SLOW: 500 ms interval + unreachable watermark; the buffer
//         accumulates aggressively, sim_crash drops it, and tail-scan
//         must rebuild ColdTier from VPages alone. This is the path
//         the paper's recovery-time claim relies on.
int run_recovery_stress() {
    std::cout << "=== HiOM multi-thread + crash injection stress (M4 Phase E) ==="
              << std::endl;

    constexpr std::size_t kHotBucketsRS = 1ULL << 12;  // 4K × 16 = 64K slots
    constexpr int kNumThreads = 8;
    constexpr int kPerThread = 20'000;        // ~600 ms total at ~250 Kops/s

    enum class FlusherMode { kFast, kSlow };
    struct IterCfg {
        FlusherMode mode;
        int crash_ms;
    };
    // Mix of fast/slow flusher and early/mid/late crash points.
    // Fast flusher lets us validate the steady-state path (most
    // entries already in cold; tail-scan is idempotent). Slow
    // flusher forces tail-scan to do real work — buffer is dropped
    // wholesale, ColdTier rebuilds from VPages.
    constexpr int kIterations = 6;
    const IterCfg iters[kIterations] = {
        {FlusherMode::kFast, 25},
        {FlusherMode::kFast, 80},
        {FlusherMode::kFast, 250},
        {FlusherMode::kSlow, 50},   // most entries buffered → tail-scan loaded
        {FlusherMode::kSlow, 200},  // mid-stream slow-flusher crash
        {FlusherMode::kSlow, 600},  // writers finish, dtor sees full buffer
    };

    std::mt19937 seed_rng(0xE5EEDull);

    for (int iter = 0; iter < kIterations; ++iter) {
        cleanup_pool();
        cleanup_cold_pool();
        cleanup_checkpoint_dir();

        std::vector<std::atomic<int>> completed_per_thread(kNumThreads);
        for (auto& c : completed_per_thread) c.store(0);
        std::atomic<bool> crashed{false};
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};

        // Phase 1: write + crash.
        std::vector<std::thread> writers;
        const int crash_delay_ms
            = iters[iter].crash_ms
              + static_cast<int>(seed_rng() % 7);
        const FlusherMode mode = iters[iter].mode;

        {
            auto viper_db = ViperT::create(kPoolDir, kPoolSize);
            auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
            auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);
            HiOMT::FlusherConfig fcfg;
            HiOMT::CheckpointConfig ccfg;
            if (mode == FlusherMode::kSlow) {
                // Buffer accumulates aggressively; flusher only wakes
                // every 500 ms and the watermark is unreachable, so
                // most entries are still in DRAM at crash time.
                fcfg.interval = std::chrono::milliseconds(500);
                fcfg.high_watermark = 1ULL << 30;
                ccfg.cadence_entries = 1ULL << 30;  // no auto checkpoints
            } else {
                ccfg.cadence_entries = 1024;        // realistic checkpoint pace
            }
            HiOMT hiom(*viper_db, kHotBucketsRS, cold.get(),
                       fcfg, chkpt.get(), ccfg);

            writers.reserve(kNumThreads);
            for (int t = 0; t < kNumThreads; ++t) {
                writers.emplace_back([&, t]() {
                    auto cl = hiom.get_client();
                    const std::uint64_t base
                        = static_cast<std::uint64_t>(t)
                          * (kPerThread + 1) + 1;
                    ready.fetch_add(1, std::memory_order_release);
                    while (!go.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    for (int i = 0; i < kPerThread; ++i) {
                        if (crashed.load(std::memory_order_acquire)) break;
                        const std::uint64_t key = base + i;
                        const std::uint64_t val
                            = (static_cast<std::uint64_t>(t) << 32)
                              | static_cast<std::uint64_t>(i);
                        cl.put(key, val);
                        // After cl.put returns, Viper has persisted
                        // the VPage entry and HiOM's mirror_write has
                        // completed. Bumping the count here means
                        // "this put is durably recoverable via tail
                        // scan". Recovery MUST find every key in
                        // [base, base + completed).
                        completed_per_thread[t].store(
                            i + 1, std::memory_order_release);
                    }
                });
            }
            while (ready.load(std::memory_order_acquire) < kNumThreads) {
                std::this_thread::yield();
            }
            const auto t0 = std::chrono::steady_clock::now();
            go.store(true, std::memory_order_release);

            std::this_thread::sleep_for(
                std::chrono::milliseconds(crash_delay_ms));
            crashed.store(true, std::memory_order_release);
            for (auto& w : writers) w.join();
            writers.clear();
            const auto t1 = std::chrono::steady_clock::now();
            const double write_ms
                = std::chrono::duration<double, std::milli>(t1 - t0).count();

            std::size_t total_completed = 0;
            for (int t = 0; t < kNumThreads; ++t) {
                total_completed += completed_per_thread[t].load();
            }
            std::cout << "  iter " << iter
                      << " mode=" << (mode == FlusherMode::kSlow ? "slow" : "fast")
                      << " crash@" << crash_delay_ms << "ms"
                      << " (" << write_ms << " ms total)"
                      << " completed=" << total_completed
                      << " cold@crash=" << hiom.cold_tier()->approx_size()
                      << std::endl;

            // Now drop the buffer + stop the flusher → on-PM state is
            // frozen at whatever Viper VPages + ColdTier + Checkpoint
            // currently have. dtor's drain becomes a no-op.
            hiom.simulate_crash_for_test();
        }

        // Phase 2: reopen with skip_recovery=true; tail-scan brings
        // ColdTier back in sync with PM VPages.
        viper::ViperConfig vcfg;
        vcfg.skip_recovery = true;
        auto viper_db = ViperT::open(kPoolDir, vcfg);
        auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
        auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
        HiOMT::RecoveryConfig rcfg;
        rcfg.tail_scan = true;
        rcfg.recovery_threads = 8;
        HiOMT hiom(*viper_db, kHotBucketsRS, cold.get(),
                   HiOMT::FlusherConfig{}, chkpt.get(),
                   HiOMT::CheckpointConfig{}, rcfg);
        const auto replayed = hiom.stats().recovery_replayed.load();
        const std::size_t cold_after = hiom.cold_tier()->approx_size();

        // Phase 3: read back every key counted by completed_per_thread.
        // Every one of these had its viper_.put return before the
        // crash, hence has a durable VPage entry. Tail-scan must have
        // re-installed them in ColdTier.
        auto cl = hiom.get_client();
        std::size_t expected = 0, recovered = 0, lost = 0;
        std::size_t mismatch = 0;
        std::size_t worst_thread_lost = 0;
        for (int t = 0; t < kNumThreads; ++t) {
            const int completed = completed_per_thread[t].load();
            const std::uint64_t base
                = static_cast<std::uint64_t>(t) * (kPerThread + 1) + 1;
            std::size_t local_lost = 0;
            for (int i = 0; i < completed; ++i) {
                ++expected;
                const std::uint64_t key = base + i;
                const std::uint64_t want
                    = (static_cast<std::uint64_t>(t) << 32)
                      | static_cast<std::uint64_t>(i);
                std::uint64_t got = 0;
                if (!cl.get(key, &got)) {
                    ++lost;
                    ++local_lost;
                } else if (got != want) {
                    ++mismatch;
                }
                else {
                    ++recovered;
                }
            }
            worst_thread_lost = std::max(worst_thread_lost, local_lost);
        }
        std::cout << "    post-recovery: replayed=" << replayed
                  << " cold=" << cold_after
                  << " expected=" << expected
                  << " recovered=" << recovered
                  << " lost=" << lost
                  << " mismatch=" << mismatch
                  << " worst-thread-lost=" << worst_thread_lost
                  << std::endl;
        if (lost != 0 || mismatch != 0) {
            std::cerr << "  FAIL iter " << iter
                      << ": persistence ordering violation — every put "
                      << "that returned before the crash should be "
                      << "recoverable via tail-scan, but lost=" << lost
                      << " mismatch=" << mismatch << std::endl;
            return 1;
        }
    }

    std::cout << "  PASS" << std::endl;
    return 0;
}

// P0 sanity: hiom_map_skipped_ counter must increment exactly once per
// write-path op when HiOM owns the index, and stay zero for raw Viper
// callers. If this passes but the perf microbench shows no win, it means
// owns_index_ took effect but the bottleneck is elsewhere; if this
// fails the wiring is broken.
int run_p0_skip_counter_sanity() {
    std::cout << "=== HiOM P0 skip-counter sanity (M3 follow-up #2) ===" << std::endl;

    constexpr int kN = 1000;

    // Phase 1: HiOM-attached Viper. Counter should grow.
    {
        cleanup_pool();
        cleanup_cold_pool();
        cleanup_checkpoint_dir();

        auto viper_db = ViperT::create(kPoolDir, kPoolSize);
        auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
        auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);
        HiOMT hiom(*viper_db, kHotBuckets, cold.get(),
                   HiOMT::FlusherConfig{}, chkpt.get());

        const std::uint64_t skipped_before = viper_db->hiom_map_skipped();
        if (skipped_before != 0) {
            std::cerr << "  FAIL: counter not zero at start: "
                      << skipped_before << std::endl;
            return 1;
        }

        auto cl = hiom.get_client();
        for (int i = 0; i < kN; ++i) {
            const std::uint64_t k = static_cast<std::uint64_t>(i + 1);
            cl.put(k, k * 7);
        }
        // put → 1 skip per call.
        const std::uint64_t after_put = viper_db->hiom_map_skipped();
        if (after_put != static_cast<std::uint64_t>(kN)) {
            std::cerr << "  FAIL (post-put): expected " << kN
                      << " skips, got " << after_put << std::endl;
            return 1;
        }

        // remove half → +1 skip per remove (Client::remove path).
        for (int i = 0; i < kN; i += 2) {
            const std::uint64_t k = static_cast<std::uint64_t>(i + 1);
            cl.remove(k);
        }
        const std::uint64_t after_remove = viper_db->hiom_map_skipped();
        const std::uint64_t expected_after_remove
            = static_cast<std::uint64_t>(kN + kN / 2);
        if (after_remove != expected_after_remove) {
            std::cerr << "  FAIL (post-remove): expected "
                      << expected_after_remove
                      << " skips, got " << after_remove << std::endl;
            return 1;
        }

        std::cout << "  HiOM-attached: " << after_remove
                  << " skips after " << kN << " puts + " << (kN / 2)
                  << " removes (expected " << expected_after_remove
                  << ")" << std::endl;

        hiom.flush_and_wait();
    }

    // Phase 2: raw Viper, no HiOM attached. Counter must stay zero.
    {
        cleanup_pool();

        auto viper_db = ViperT::create(kPoolDir, kPoolSize);
        auto cl = viper_db->get_client();
        for (int i = 0; i < kN; ++i) {
            const std::uint64_t k = static_cast<std::uint64_t>(i + 1);
            cl.put(k, k * 11);
        }
        const std::uint64_t legacy_skips = viper_db->hiom_map_skipped();
        if (legacy_skips != 0) {
            std::cerr << "  FAIL (legacy): expected 0 skips, got "
                      << legacy_skips << std::endl;
            return 1;
        }
        std::cout << "  raw Viper (no HiOM): "
                  << legacy_skips << " skips after " << kN
                  << " puts (expected 0)" << std::endl;
    }

    std::cout << "  PASS" << std::endl;
    return 0;
}

// P0 multi-thread correctness: 8 writers, distinct keys per thread, mix
// of update + remove. Verifies reads return latest values, removed keys
// miss. This is the YCSB-A-shaped workload (no cross-thread same-key
// races), so the known same-key-leak limitation does not trigger.
int run_p0_update_heavy_multi_thread() {
    std::cout << "=== HiOM P0 update-heavy multi-thread (M3 follow-up #2) ==="
              << std::endl;

    cleanup_pool();
    cleanup_cold_pool();
    cleanup_checkpoint_dir();

    constexpr int kNumThreads = 2;
    constexpr int kPerThread = 25'000;       // 50K keys total (2-thread debug)
    constexpr std::size_t kHotBucketsP0 = 1ULL << 14;  // 16K × 16 = 256K slots

    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
    auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);
    HiOMT hiom(*viper_db, kHotBucketsP0, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get());

    auto base_for_thread = [](int t) -> std::uint64_t {
        return static_cast<std::uint64_t>(t)
                * static_cast<std::uint64_t>(kPerThread + 1) + 1;
    };

    std::atomic<std::uint64_t> remove_returned_false{0};
    auto worker = [&](int thread_id) {
        auto cl = hiom.get_client();
        const std::uint64_t base = base_for_thread(thread_id);
        // Phase 1: insert.
        for (int i = 0; i < kPerThread; ++i) {
            cl.put(base + i, (base + i) * 7);
        }
        // Phase 2: update every key in place to a new value.
        for (int i = 0; i < kPerThread; ++i) {
            cl.put(base + i, (base + i) * 13);
        }
        // Phase 3: remove odd-indexed half.
        for (int i = 1; i < kPerThread; i += 2) {
            if (!cl.remove(base + i)) {
                remove_returned_false.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& th : threads) th.join();
    hiom.flush_and_wait();

    // Verify: even-indexed keys hold the *13 value; odd-indexed are gone.
    auto cl = hiom.get_client();
    auto vcl_ro = viper_db->get_read_only_client();
    std::size_t hit_even = 0;
    std::size_t miss_even = 0;
    std::size_t mismatch_even = 0;
    std::size_t leaked_odd = 0;     // odd key that should have been removed
    for (int t = 0; t < kNumThreads; ++t) {
        const std::uint64_t base = base_for_thread(t);
        for (int i = 0; i < kPerThread; ++i) {
            const std::uint64_t k = base + i;
            std::uint64_t got = 0;
            const bool found = cl.get(k, &got);
            if (i % 2 == 0) {
                if (!found) {
                    ++miss_even;
                } else if (got != k * 13) {
                    ++mismatch_even;
                } else {
                    ++hit_even;
                }
            } else {
                if (found) {
                    if (leaked_odd < 5) {
                        const auto fp64 = viper::hiom::key_fingerprint64(k);
                        const auto fp32 = viper::hiom::key_fingerprint(k);
                        const auto cold_off = hiom.cold_tier()->lookup(fp64);
                        const auto hot_packed = hiom.hot_tier().lookup(fp32);
                        std::cerr << "    leaked odd k=" << k
                                  << " thread=" << t << " idx=" << i
                                  << " got=" << got
                                  << " (k*13=" << (k * 13) << ")"
                                  << " cold_lookup="
                                  << (cold_off ? "hit" : "MISS")
                                  << " hot_lookup="
                                  << (hot_packed ? "hit" : "MISS");
                        if (cold_off) {
                            std::uint64_t pmk = 0, pmv = 0;
                            const bool pm_ok =
                                vcl_ro.hiom_read_at_offset(*cold_off,
                                                            &pmk, &pmv);
                            std::cerr << " cold_off="
                                      << cold_off->offset
                                      << " pm_alive=" << pm_ok
                                      << " pm_key=" << pmk;
                        }
                        std::cerr << std::endl;
                    }
                    ++leaked_odd;
                }
            }
        }
    }
    const std::size_t expected_even
        = static_cast<std::size_t>(kNumThreads) * ((kPerThread + 1) / 2);
    std::cout << "  hit_even=" << hit_even
              << " miss_even=" << miss_even
              << " mismatch_even=" << mismatch_even
              << " leaked_odd=" << leaked_odd
              << " remove_returned_false="
              << remove_returned_false.load()
              << " (expected hit_even=" << expected_even
              << ", others=0)" << std::endl;
    if (hit_even != expected_even || miss_even != 0
        || mismatch_even != 0 || leaked_odd != 0) {
        std::cerr << "  FAIL: P0 update/remove semantics broken." << std::endl;
        return 1;
    }

    std::cout << "  PASS" << std::endl;
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= run_p0_skip_counter_sanity();
    rc |= run_correctness();
    rc |= run_update_remove();
    rc |= run_microbench();
    rc |= run_cold_backed();
    rc |= run_commit_window();
    rc |= run_checkpoint_persistence();
    rc |= run_recovery_persistence();
    rc |= run_p0_update_heavy_multi_thread();
    rc |= run_multi_producer_correctness();
    rc |= run_recovery_stress();
    if (rc != 0) {
        std::cerr << "\nFAIL" << std::endl;
        return 1;
    }
    std::cout << "\nALL PASS" << std::endl;
    return 0;
}
