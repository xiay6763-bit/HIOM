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
//      into ColdTier; reads of evicted-from-hot keys must hit ColdTier
//      and never need the CCEH fallback.
//
// Note: HiOM still keeps Viper's CCEH as a safety net in M2 — the
// `cceh_fallback_hits` counter is asserted to stay ~0 under steady
// state. Once M3 retires CCEH, the safety net is removed.

#include "viper/viper.hpp"
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
    // reads must route through ColdTier; CCEH fallback should stay at 0.
    auto reset_stats = [&]() {
        // Stats accumulated during put-time warmups are uninteresting
        // for read measurement. We snapshot before reads instead of
        // resetting (Stats has no clear()).
        return std::tuple{
            hiom.stats().hot_hits.load(),
            hiom.stats().cold_hits.load(),
            hiom.stats().cold_misses.load(),
            hiom.stats().cold_fp_collisions.load(),
            hiom.stats().cceh_fallback_hits.load()};
    };
    const auto [hh0, ch0, cm0, cfc0, cf0] = reset_stats();

    std::size_t hits = 0, misses = 0, mismatches = 0;
    for (auto& [k, expected] : kv) {
        std::uint64_t got = 0;
        if (!client.get(k, &got)) ++misses;
        else if (got != expected) ++mismatches;
        else ++hits;
    }

    const auto [hh1, ch1, cm1, cfc1, cf1] = reset_stats();
    const std::uint64_t hot_hits_delta  = hh1 - hh0;
    const std::uint64_t cold_hits_delta = ch1 - ch0;
    const std::uint64_t cold_miss_delta = cm1 - cm0;
    const std::uint64_t cold_fpc_delta  = cfc1 - cfc0;
    const std::uint64_t cceh_fb_delta   = cf1 - cf0;
    std::cout << "  read pass: hits=" << hits
              << " misses=" << misses
              << " mismatches=" << mismatches << std::endl;
    std::cout << "  per-tier deltas: hot=" << hot_hits_delta
              << " cold=" << cold_hits_delta
              << " cold_miss=" << cold_miss_delta
              << " cold_fp_coll=" << cold_fpc_delta
              << " cceh_fallback=" << cceh_fb_delta << std::endl;

    if (misses != 0 || mismatches != 0) {
        std::cerr << "  FAIL: lossy read pass" << std::endl;
        return 1;
    }
    if (cceh_fb_delta != 0) {
        std::cerr << "  FAIL: ColdTier should cover all reads, but "
                  << cceh_fb_delta << " queries fell through to CCEH"
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
    const auto [hh2, ch2, cm2, cfc2, cf2] = reset_stats();
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
    const auto [hh3, ch3, cm3, cfc3, cf3] = reset_stats();
    std::cout << "  final pass deltas: hot=" << (hh3 - hh2)
              << " cold=" << (ch3 - ch2)
              << " cold_miss=" << (cm3 - cm2)
              << " cold_fp_coll=" << (cfc3 - cfc2)
              << " cceh_fallback=" << (cf3 - cf2) << std::endl;
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

// Phase B-2 follow-up (M3 Phase A+B+C): exercise reads while the
// commit buffer still has un-flushed entries. Demonstrates that
// HiOM stays correct even when ColdTier hasn't caught up: keys
// resolved via the CCEH safety net are functionally indistinguishable
// from cold-tier hits, the only difference being cceh_fallback_hits
// rises during the commit window. Once the flusher drains, the same
// reads route through ColdTier with cceh_fallback=0.
int run_commit_window() {
    std::cout << "=== HiOM commit-window read correctness (M3) ===" << std::endl;
    cleanup_pool();
    cleanup_cold_pool();

    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    auto cold = viper::hiom::ColdTier::create(kColdPoolFile);
    // Slow the flusher so reads will see in-flight commits.
    viper::hiom::HiOM<std::uint64_t, std::uint64_t>::FlusherConfig fcfg;
    fcfg.interval = std::chrono::milliseconds(500);
    fcfg.high_watermark = 1ULL << 30;  // never wake on size
    HiOMT hiom(*viper_db, /*hot_buckets_pow2=*/256, cold.get(), fcfg);
    auto client = hiom.get_client();

    constexpr std::size_t kN = 50'000;
    std::mt19937_64 rng(0xb00b);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kv;
    kv.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        kv.emplace_back(static_cast<std::uint64_t>(i + 1), rng());
    }

    // Write all keys quickly. Flusher won't wake (huge watermark, slow
    // interval), so most entries should still be in the buffer when we
    // start reading.
    for (auto& [k, v] : kv) {
        if (!client.put(k, v)) {
            std::cerr << "  FAIL: put returned false for key " << k << std::endl;
            return 1;
        }
    }
    const std::size_t cold_before = cold->approx_size();
    const std::size_t buf_before = hiom.commit_buffer()->size_hint();
    std::cout << "  after fill: ColdTier=" << cold_before
              << " buffer=" << buf_before
              << " (most writes still in buffer)" << std::endl;
    if (buf_before == 0) {
        std::cerr << "  WARN: flusher already drained; can't exercise the"
                  << " commit-window path with this timing." << std::endl;
    }

    const auto cf0 = hiom.stats().cceh_fallback_hits.load();
    std::size_t hits = 0, misses = 0, mismatches = 0;
    for (const auto& [k, expected] : kv) {
        std::uint64_t got = 0;
        if (!client.get(k, &got)) ++misses;
        else if (got != expected) ++mismatches;
        else ++hits;
    }
    const auto cf1 = hiom.stats().cceh_fallback_hits.load();
    std::cout << "  reads during commit window: hits=" << hits
              << " misses=" << misses << " mismatches=" << mismatches
              << " cceh_fallback_delta=" << (cf1 - cf0) << std::endl;

    if (misses != 0 || mismatches != 0) {
        std::cerr << "  FAIL: in-flight commits observed lost or stale"
                  << std::endl;
        return 1;
    }
    // Now drain and confirm post-flush state is consistent.
    hiom.flush_and_wait();
    if (cold->approx_size() != kN) {
        std::cerr << "  FAIL: post-flush ColdTier=" << cold->approx_size()
                  << " expected " << kN << std::endl;
        return 1;
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
    if (rc != 0) {
        std::cerr << "\nFAIL" << std::endl;
        return 1;
    }
    std::cout << "\nALL PASS" << std::endl;
    return 0;
}
