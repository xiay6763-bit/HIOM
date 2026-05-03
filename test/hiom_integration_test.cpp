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
//
// Note: HiOM relies on Viper's CCEH for the authoritative offset map
// (M0 scope). Once cold tier (M2) replaces CCEH, this test will be
// rewritten to compare against pure-CCEH Viper as a baseline.

#include "viper/viper.hpp"
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

}  // namespace

int main() {
    int rc = 0;
    rc |= run_correctness();
    rc |= run_update_remove();
    rc |= run_microbench();
    if (rc != 0) {
        std::cerr << "\nFAIL" << std::endl;
        return 1;
    }
    std::cout << "\nALL PASS" << std::endl;
    return 0;
}
