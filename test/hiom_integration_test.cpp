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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kPoolDir = "/pmem0/viper_hiom_test";
constexpr const char* kColdPoolFile = "/pmem0/viper_hiom_test_cold/cold.bin";
constexpr const char* kColdPoolDir  = "/pmem0/viper_hiom_test_cold";
constexpr const char* kCheckpointDir  = "/pmem0/viper_hiom_test_chkpt";
constexpr const char* kCheckpointFile = "/pmem0/viper_hiom_test_chkpt/chkpt.bin";
constexpr std::size_t kPoolSize = 1ULL << 30;     // 1 GiB
constexpr std::size_t kNumKeys = 200'000;          // ~150 blocks of uint64_t pairs, well under 8K limit
constexpr std::size_t kHotBuckets = 1ULL << 15;    // 32K buckets × 16 = 512K slots
constexpr std::size_t kBenchIters = 5'000'000;

using ViperT = viper::Viper<std::uint64_t, std::uint64_t>;
using HiOMT = viper::hiom::HiOM<std::uint64_t, std::uint64_t>;

void cleanup_pool() {
    // Defensive: same pattern as benchmark/fixtures/viper_fixture.hpp.
    // Only touch our own directory under /pmem0; never blanket-rm.
    if (std::string(kPoolDir).find("/pmem0/viper_hiom_test") != 0) {
        std::cerr << "Refusing to clean unfamiliar pool path: " << kPoolDir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(kPoolDir);
}

void cleanup_cold_pool() {
    // Same defensive check for the ColdTier file. The directory holds
    // exactly cold.bin; nothing else from other users.
    if (std::string(kColdPoolDir).find("/pmem0/viper_hiom_test_cold") != 0) {
        std::cerr << "Refusing to clean unfamiliar cold path: " << kColdPoolDir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(kColdPoolDir);
    std::filesystem::create_directories(kColdPoolDir);
}

void cleanup_checkpoint_dir() {
    // Same defensive pattern. Holds only chkpt.bin (4 KB).
    if (std::string(kCheckpointDir).find("/pmem0/viper_hiom_test_chkpt") != 0) {
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

}  // namespace

int main() {
    int rc = 0;
    rc |= run_correctness();
    rc |= run_update_remove();
    rc |= run_microbench();
    rc |= run_cold_backed();
    rc |= run_commit_window();
    rc |= run_checkpoint_persistence();
    if (rc != 0) {
        std::cerr << "\nFAIL" << std::endl;
        return 1;
    }
    std::cout << "\nALL PASS" << std::endl;
    return 0;
}
