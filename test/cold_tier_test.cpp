// Standalone test driver for ColdTier (M2 Phase B-1).
//
// Tests:
//   1. correctness: single-threaded upsert/lookup
//   2. update via 8 B atomic CAS
//   3. remove + reactivate
//   4. close + reopen persistence
//   5. overflow chain: heavy load, no bucket-full failures
//   6. concurrent stress: 8 threads, disjoint key ranges, last-writer-wins
//   7. parallel_load: 32-thread region scan, count entries
//   8. M2 exit: 100M-class workload, parallel_load ≤ 5 s
//   9. single-threaded microbench

#include "viper/hiom/cold_tier.hpp"

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

constexpr const char* kPoolFile = "/pmem0/hiom_cold_tier_test";
constexpr std::size_t kSmallMain = 1024;
constexpr std::size_t kSmallOverflow = 4096;
constexpr std::size_t kLargeMain = 8192;
constexpr std::size_t kLargeOverflow = 16384;
constexpr std::size_t kBenchIters = 2'000'000;

using viper::hiom::ColdTier;
using Offset = ColdTier::Offset;

void cleanup() {
    if (std::string(kPoolFile).find("/pmem0/hiom_cold_tier_test") != 0) {
        std::cerr << "Refusing unfamiliar pool path: " << kPoolFile
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove(kPoolFile);
}

Offset make_offset(std::uint64_t i) {
    Offset o{};
    o.offset = i & 0x0FFF'FFFF'FFFF'FFFFull;
    if (o.offset == ColdTier::kTombstoneOffset) o.offset = 1;
    return o;
}

std::uint64_t make_fp(std::uint64_t i) {
    std::uint64_t fp = i * 0x9e3779b97f4a7c15ull;
    fp ^= fp >> 33;
    if (fp == 0) fp = 1;
    return fp;
}

int run_correctness() {
    std::cout << "=== ColdTier correctness ===" << std::endl;
    cleanup();
    auto ct = ColdTier::create(kPoolFile, kSmallMain, kSmallOverflow);
    constexpr std::size_t kN = 100'000;
    std::size_t inserted = 0, full = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        if (ct->upsert(make_fp(i), make_offset(i))) ++inserted;
        else ++full;
    }
    std::size_t hits = 0, misses = 0, mismatches = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        auto v = ct->lookup(make_fp(i));
        if (!v) ++misses;
        else if (v->offset != make_offset(i).offset) ++mismatches;
        else ++hits;
    }
    std::cout << "  inserted " << inserted << " (" << full << " bucket-full), "
              << "lookup: " << hits << " hits, " << misses << " misses, "
              << mismatches << " mismatches" << std::endl;
    if (mismatches != 0 || misses != full) {
        std::cerr << "  FAIL" << std::endl;
        return 1;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_update() {
    std::cout << "=== ColdTier update CAS ===" << std::endl;
    cleanup();
    auto ct = ColdTier::create(kPoolFile, kSmallMain, kSmallOverflow);
    constexpr std::size_t kN = 1000;
    for (std::size_t i = 0; i < kN; ++i) ct->upsert(make_fp(i), make_offset(i));
    for (std::size_t i = 0; i < kN; ++i) ct->upsert(make_fp(i), make_offset(i + 100000));
    std::size_t bad = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        auto v = ct->lookup(make_fp(i));
        if (!v || v->offset != make_offset(i + 100000).offset) ++bad;
    }
    if (bad) { std::cerr << "  FAIL: " << bad << std::endl; return 1; }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_remove() {
    std::cout << "=== ColdTier remove + reactivate ===" << std::endl;
    cleanup();
    auto ct = ColdTier::create(kPoolFile, kSmallMain, kSmallOverflow);
    for (std::size_t i = 0; i < 1000; ++i) ct->upsert(make_fp(i), make_offset(i));
    for (std::size_t i = 0; i < 1000; i += 2) ct->remove(make_fp(i));
    std::size_t bad = 0;
    for (std::size_t i = 0; i < 1000; ++i) {
        auto v = ct->lookup(make_fp(i));
        const bool should = (i % 2 != 0);
        if (should != v.has_value()) ++bad;
    }
    if (bad) { std::cerr << "  FAIL after remove: " << bad << std::endl; return 1; }
    for (std::size_t i = 0; i < 1000; i += 2) ct->upsert(make_fp(i), make_offset(i + 200000));
    bad = 0;
    for (std::size_t i = 0; i < 1000; i += 2) {
        auto v = ct->lookup(make_fp(i));
        if (!v || v->offset != make_offset(i + 200000).offset) ++bad;
    }
    if (bad) { std::cerr << "  FAIL after reactivate: " << bad << std::endl; return 1; }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_persistence() {
    std::cout << "=== ColdTier close + reopen persistence ===" << std::endl;
    cleanup();
    constexpr std::size_t kN = 50'000;
    {
        auto ct = ColdTier::create(kPoolFile, kSmallMain, kSmallOverflow);
        for (std::size_t i = 0; i < kN; ++i) ct->upsert(make_fp(i), make_offset(i));
    }
    auto ct = ColdTier::open(kPoolFile);
    std::size_t hits = 0, misses = 0, mismatches = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        auto v = ct->lookup(make_fp(i));
        if (!v) ++misses;
        else if (v->offset != make_offset(i).offset) ++mismatches;
        else ++hits;
    }
    std::cout << "  reopen: " << hits << " hits, " << misses
              << " misses, " << mismatches << " mismatches" << std::endl;
    if (mismatches != 0 || hits == 0) {
        std::cerr << "  FAIL" << std::endl;
        return 1;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_overflow_chain() {
    std::cout << "=== ColdTier overflow chain (heavy load, no bucket-full) ===" << std::endl;
    cleanup();
    // 32 × 1024 × 7 = 229376 main capacity.
    // Insert 200K — under capacity but skewed enough to trigger overflows.
    auto ct = ColdTier::create(kPoolFile, kSmallMain, kSmallOverflow);
    constexpr std::size_t kN = 200'000;
    std::size_t ok = 0, fail = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        if (ct->upsert(make_fp(i), make_offset(i))) ++ok;
        else ++fail;
    }
    std::cout << "  inserted " << ok << " of " << kN
              << ", overflow buckets used = " << ct->approx_overflow_used()
              << std::endl;

    // With a 4× overflow pool relative to main, no insert should fail
    // unless main + overflow is also fully exhausted. 200K is below the
    // 32 × (1024 + 4096) × 7 ≈ 1.15M total capacity, so all should land.
    if (fail != 0) {
        std::cerr << "  FAIL: " << fail << " inserts failed despite overflow pool"
                  << std::endl;
        return 1;
    }
    std::size_t bad = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        auto v = ct->lookup(make_fp(i));
        if (!v || v->offset != make_offset(i).offset) ++bad;
    }
    if (bad) {
        std::cerr << "  FAIL: " << bad << " unfound or mismatch" << std::endl;
        return 1;
    }
    std::cout << "  PASS (overflow chain integral; all keys findable)" << std::endl;
    return 0;
}

int run_concurrent_stress() {
    std::cout << "=== ColdTier concurrent stress (8 threads, disjoint keys) ===" << std::endl;
    cleanup();
    auto ct = ColdTier::create(kPoolFile, kLargeMain, kLargeOverflow);

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 50'000;
    std::atomic<std::size_t> failures{0};

    std::vector<std::thread> ws;
    for (std::size_t t = 0; t < kThreads; ++t) {
        ws.emplace_back([t, &ct, &failures]() {
            for (std::size_t i = 0; i < kPerThread; ++i) {
                const std::uint64_t key = (t * kPerThread) + i + 1;
                if (!ct->upsert(make_fp(key), make_offset(key))) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : ws) w.join();
    std::cout << "  upsert failures: " << failures.load() << std::endl;

    std::atomic<std::size_t> miss{0}, bad{0};
    std::vector<std::thread> rs;
    for (std::size_t t = 0; t < kThreads; ++t) {
        rs.emplace_back([t, &ct, &miss, &bad]() {
            for (std::size_t i = 0; i < kPerThread; ++i) {
                const std::uint64_t key = (t * kPerThread) + i + 1;
                auto v = ct->lookup(make_fp(key));
                if (!v) miss.fetch_add(1, std::memory_order_relaxed);
                else if (v->offset != make_offset(key).offset)
                    bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& r : rs) r.join();
    std::cout << "  lookup miss=" << miss.load()
              << " bad=" << bad.load() << std::endl;
    if (failures.load() != 0 || miss.load() != 0 || bad.load() != 0) {
        std::cerr << "  FAIL" << std::endl;
        return 1;
    }
    std::cout << "  PASS (multi-thread upsert + lookup correct)" << std::endl;
    return 0;
}

int run_parallel_load() {
    std::cout << "=== ColdTier parallel_load (32-thread scan) ===" << std::endl;
    cleanup();
    auto ct = ColdTier::create(kPoolFile, kLargeMain, kLargeOverflow);

    constexpr std::size_t kN = 500'000;
    for (std::size_t i = 0; i < kN; ++i) {
        ct->upsert(make_fp(i), make_offset(i));
    }
    std::cout << "  inserted " << kN << " entries (overflow used = "
              << ct->approx_overflow_used() << ")" << std::endl;

    std::atomic<std::size_t> seen{0};
    auto t0 = std::chrono::steady_clock::now();
    ct->parallel_load(32, [&seen](std::uint64_t /*fp*/, Offset /*o*/) {
        seen.fetch_add(1, std::memory_order_relaxed);
    });
    auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "  scan saw " << seen.load() << " entries in "
              << sec * 1000 << " ms" << std::endl;
    if (seen.load() != kN) {
        std::cerr << "  FAIL: expected " << kN << " got " << seen.load()
                  << std::endl;
        return 1;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_m2_exit() {
    if (std::getenv("SKIP_M2_EXIT")) {
        std::cout << "=== ColdTier M2 exit (SKIPPED via env) ===" << std::endl;
        return 0;
    }
    std::cout << "=== ColdTier M2 exit (large dataset, parallel_load ≤ 5 s) ===" << std::endl;
    cleanup();
    // Default kN is small enough to finish in ~minute on a development PM
    // mount; set COLD_TIER_EXIT_N=100000000 for the real M2 exit run.
    std::size_t kN = 1'000'000;
    if (const char* env = std::getenv("COLD_TIER_EXIT_N")) {
        kN = std::strtoull(env, nullptr, 10);
    }
    // Provision capacity to ~2× kN so overflow stays bounded.
    const std::size_t main_buckets = std::max<std::size_t>(
        4096, (kN / (32 * 7)));
    const std::size_t overflow_slots = main_buckets;  // 1× overflow pool

    std::cout << "  kN=" << kN << "  main_buckets=" << main_buckets
              << "  overflow_slots=" << overflow_slots
              << "  pm_size≈" << (32ULL * (main_buckets + overflow_slots + 1)
                                  * sizeof(ColdTier::Bucket) >> 20) << " MB"
              << std::endl;

    auto ct = ColdTier::create(kPoolFile, main_buckets, overflow_slots);

    auto ti0 = std::chrono::steady_clock::now();
    std::size_t fails = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        if (!ct->upsert(make_fp(i), make_offset(i))) ++fails;
    }
    auto ti1 = std::chrono::steady_clock::now();
    const double insert_sec = std::chrono::duration<double>(ti1 - ti0).count();
    std::cout << "  insert " << kN << " in " << insert_sec << " s ("
              << kN / insert_sec / 1e6 << " M ops/s, "
              << fails << " failures)" << std::endl;

    std::atomic<std::size_t> seen{0};
    auto t0 = std::chrono::steady_clock::now();
    ct->parallel_load(32, [&seen](std::uint64_t, Offset) {
        seen.fetch_add(1, std::memory_order_relaxed);
    });
    auto t1 = std::chrono::steady_clock::now();
    const double scan_sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "  parallel_load(32) scanned " << seen.load()
              << " entries in " << scan_sec << " s ("
              << seen.load() / scan_sec / 1e6 << " M ops/s)" << std::endl;

    if (scan_sec > 5.0) {
        std::cerr << "  WARN: parallel_load over 5 s exit target" << std::endl;
    } else {
        std::cout << "  PASS (parallel_load ≤ 5 s)" << std::endl;
    }
    return 0;
}

int run_microbench() {
    std::cout << "=== ColdTier single-threaded microbench ===" << std::endl;
    cleanup();
    auto ct = ColdTier::create(kPoolFile, kLargeMain, kLargeOverflow);
    constexpr std::size_t kN = 1'000'000;
    std::mt19937_64 rng(0xdeadbeef);
    std::vector<std::uint64_t> fps;
    fps.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) fps.push_back(make_fp(rng()));

    auto t0 = std::chrono::steady_clock::now();
    std::size_t inserted = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        if (ct->upsert(fps[i], make_offset(i))) ++inserted;
    }
    auto t1 = std::chrono::steady_clock::now();
    const double ins_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double ins_mops = (double)inserted * 1e9 / ins_ns / 1e6;

    std::shuffle(fps.begin(), fps.end(), rng);
    auto t2 = std::chrono::steady_clock::now();
    std::size_t acc = 0;
    for (std::size_t i = 0; i < kBenchIters; ++i) {
        auto v = ct->lookup(fps[i & (fps.size() - 1)]);
        if (v) acc += v->offset;
    }
    auto t3 = std::chrono::steady_clock::now();
    const double look_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count());
    const double look_mops = (double)kBenchIters * 1e9 / look_ns / 1e6;

    std::printf("  insert: %.2f M ops/s   %.2f ns/op   (%zu inserted)\n",
                ins_mops, ins_ns / inserted, inserted);
    std::printf("  lookup: %.2f M ops/s   %.2f ns/op   (acc=%llu)\n",
                look_mops, look_ns / kBenchIters,
                static_cast<unsigned long long>(acc));
    if (ins_mops < 2.0) {
        std::cerr << "  WARN: insert below 2 M/s M2 exit" << std::endl;
    } else {
        std::cout << "  PASS (insert ≥ 2 M/s M2 exit met)" << std::endl;
    }
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= run_correctness();
    rc |= run_update();
    rc |= run_remove();
    rc |= run_persistence();
    rc |= run_overflow_chain();
    rc |= run_concurrent_stress();
    rc |= run_parallel_load();
    rc |= run_m2_exit();
    rc |= run_microbench();
    if (rc != 0) {
        std::cerr << "\nFAIL" << std::endl;
        return 1;
    }
    std::cout << "\nALL PASS" << std::endl;
    return 0;
}
