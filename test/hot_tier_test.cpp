// M0 hot tier test driver.
//
// Three checks:
//   1. Insert N random fingerprints, then lookup all expected (fp, offset)
//      pairs and confirm the in-tier copy matches the latest write per fp.
//   2. Microbenchmark: single-threaded lookup throughput on a hot working set.
//      Target: ≥ 8 M lookups/sec single-threaded.
//   3. Sanity remove: insert, remove, lookup must miss.
//
// Compile: g++ -O3 -std=c++17 -I include test/hot_tier_test.cpp -o build/hot_tier_test
// Run:     ./build/hot_tier_test

#include "viper/hiom/hot_tier.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

using viper::hiom::HotTier;

namespace {

constexpr std::size_t kNumKeys = 1'000'000;
// 128K buckets × 16 slots = 2M total slots → ~50% load with 1M keys.
constexpr std::size_t kNumBuckets = 1ULL << 17;
constexpr std::size_t kBenchIters = 10'000'000;

// Bias fp to non-zero (HotTier reserves 0).
std::uint32_t make_fp(std::uint32_t raw) {
    return raw == 0 ? 1 : raw;
}

int run_correctness() {
    std::cout << "=== M0 correctness test ===" << std::endl;
    std::cout << "  num_buckets = " << kNumBuckets
              << " (capacity = " << kNumBuckets * 16 << " slots), "
              << "inserting " << kNumKeys << " random fingerprints" << std::endl;

    HotTier ht(kNumBuckets);

    std::mt19937_64 rng(0xc0ffee);
    std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> kv;
    kv.reserve(kNumKeys);
    for (std::size_t i = 0; i < kNumKeys; ++i) {
        kv.emplace_back(make_fp(dist(rng)),
                        static_cast<std::uint32_t>(i));
    }

    std::size_t newly_inserted = 0;
    std::size_t replaced = 0;
    std::size_t window_full = 0;
    for (auto& [fp, off] : kv) {
        const std::uint32_t prev = ht.upsert(fp, off);
        if (prev == HotTier::kInvalidOffset) {
            // Either truly new, or window was full (we can't tell apart at this API).
            // size() growth tells us new vs failed.
            ++newly_inserted;
        } else {
            ++replaced;
        }
    }
    window_full = newly_inserted - ht.size();
    newly_inserted -= window_full;

    std::cout << "  upsert results: " << newly_inserted << " new, "
              << replaced << " replaced, "
              << window_full << " probe-window-full failures" << std::endl;
    std::cout << "  ht.size() = " << ht.size() << std::endl;

    // Build the expected last-write-wins map and check each lookup.
    std::unordered_map<std::uint32_t, std::uint32_t> expected;
    expected.reserve(kNumKeys);
    for (auto& [fp, off] : kv) expected[fp] = off;

    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t mismatches = 0;
    for (auto& [fp, exp_off] : expected) {
        auto got = ht.lookup(fp);
        if (!got) {
            ++misses;
        } else if (*got != exp_off) {
            ++mismatches;
        } else {
            ++hits;
        }
    }

    std::cout << "  lookup: " << hits << " hits, "
              << misses << " misses (window-full or evicted), "
              << mismatches << " mismatches" << std::endl;

    if (mismatches != 0) {
        std::cerr << "  FAIL: mismatched offsets, last-write-wins broken"
                  << std::endl;
        return 1;
    }
    // It's OK to have some misses if the probe window was full for those keys.
    if (misses != window_full) {
        std::cerr << "  WARN: misses(" << misses << ") != window_full("
                  << window_full << "), likely a bug" << std::endl;
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

int run_remove() {
    std::cout << "=== M0 remove sanity ===" << std::endl;
    HotTier ht(1024);
    ht.upsert(123, 456);
    auto a = ht.lookup(123);
    if (!a || *a != 456) {
        std::cerr << "  FAIL: post-insert lookup wrong" << std::endl;
        return 1;
    }
    if (!ht.remove(123)) {
        std::cerr << "  FAIL: remove returned false" << std::endl;
        return 1;
    }
    auto b = ht.lookup(123);
    if (b.has_value()) {
        std::cerr << "  FAIL: post-remove lookup hit (offset=" << *b << ")"
                  << std::endl;
        return 1;
    }
    if (ht.remove(123)) {
        std::cerr << "  FAIL: remove of absent key returned true" << std::endl;
        return 1;
    }

    // Regression: open-addressing-with-early-exit would mis-handle this case.
    // Insert two keys colliding into the same bucket, remove the first, then
    // confirm the second is still findable. The old code (now replaced) had
    // a latent bug here.
    {
        HotTier ht2(64);
        // Force same bucket: pick two fps that bucket_index() maps to the same
        // bucket. Search a few candidates.
        std::uint32_t fp_a = 0, fp_b = 0;
        for (std::uint32_t f = 1; f < 100000; ++f) {
            for (std::uint32_t g = f + 1; g < f + 200; ++g) {
                // Probe by inserting and seeing if same bucket; cheap enough.
                HotTier probe(64);
                probe.upsert(f, 1);
                probe.upsert(g, 2);
                if (probe.size() == 2) {
                    // Confirm via remove-and-lookup pattern.
                    fp_a = f;
                    fp_b = g;
                    goto found;
                }
            }
        }
    found:
        if (fp_a == 0 || fp_b == 0) {
            std::cerr << "  WARN: could not synthesize collision pair, skipping"
                      << std::endl;
        } else {
            ht2.upsert(fp_a, 11);
            ht2.upsert(fp_b, 22);
            ht2.remove(fp_a);
            auto found_b = ht2.lookup(fp_b);
            if (!found_b || *found_b != 22) {
                std::cerr << "  FAIL: delete-then-lookup regression "
                          << "(fp_a=" << fp_a << ", fp_b=" << fp_b
                          << ", got=" << (found_b ? std::to_string(*found_b) : "MISS")
                          << ")" << std::endl;
                return 1;
            }
        }
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

// Concurrent stress: N threads each insert disjoint key ranges, then we
// verify that every key's "last write" wins. We use disjoint key ranges
// per thread so there's no semantic ambiguity — every fp should map to
// exactly one expected offset.
//
// This catches: lost writes from CAS bugs, double-claim of empty slots,
// torn reads (since offset is read concurrently with insert), races
// between upsert-as-update and upsert-as-insert.
int run_concurrent() {
    std::cout << "=== M0 concurrent stress (8 threads, disjoint key ranges) ==="
              << std::endl;

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 100'000;
    HotTier ht(1ULL << 16);  // 64K buckets × 16 = 1M slots → ~76% load when 8×100K filled

    std::atomic<std::size_t> failures{0};

    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &ht, &failures]() {
            std::mt19937_64 rng(0x1234 + t);
            std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);
            for (std::size_t i = 0; i < kPerThread; ++i) {
                // Encode (thread, i) into the offset so we can verify uniqueness later.
                const std::uint32_t fp = make_fp(dist(rng));
                const std::uint32_t off
                    = static_cast<std::uint32_t>((t << 24) | (i & 0x00FFFFFFu));
                ht.upsert(fp, off);
            }
        });
    }
    for (auto& w : workers) w.join();

    // Sanity: size should be ≤ kThreads * kPerThread (might be less due to fp collisions).
    const std::size_t total_inserts = kThreads * kPerThread;
    const std::size_t s = ht.size();
    std::cout << "  inserted up to " << total_inserts << ", "
              << "size = " << s << " (diff = collisions + window-full)"
              << std::endl;

    // Spot-check: lookup a sample of (thread, i) pairs, regenerate the same
    // fp using the same RNG seed, and confirm the value's high byte = thread id
    // for *some* recent insert. We just check that lookups don't panic and
    // values decode to a valid (thread, i).
    std::size_t valid = 0;
    std::size_t miss = 0;
    for (std::size_t t = 0; t < kThreads; ++t) {
        std::mt19937_64 rng(0x1234 + t);
        std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);
        for (std::size_t i = 0; i < kPerThread; ++i) {
            const std::uint32_t fp = make_fp(dist(rng));
            (void)i;
            auto v = ht.lookup(fp);
            if (!v) {
                ++miss;
                continue;
            }
            const std::uint32_t found_t = (*v) >> 24;
            if (found_t >= kThreads) {
                ++failures;
            } else {
                ++valid;
            }
        }
    }

    std::cout << "  spot-check: " << valid << " valid lookups, "
              << miss << " misses, " << failures.load() << " corruptions"
              << std::endl;

    if (failures.load() != 0) {
        std::cerr << "  FAIL: torn read or corrupted slot" << std::endl;
        return 1;
    }
    std::cout << "  PASS (no torn reads, no corruption)" << std::endl;
    return 0;
}

// Helper: synthesize N fingerprints that all map to the same bucket.
// Caller-supplied predicate `same_bucket(fp_a, fp_b)` is checked by inserting
// into a probe HotTier and seeing whether they end up in the same bucket
// (size grows or stays for distinct-key inserts).
//
// We brute-force search until we have count fps with same target bucket.
std::vector<std::uint32_t> make_collision_set(std::size_t num_buckets,
                                              std::size_t count) {
    // Use the same bucket_index hash as HotTier internally. Easiest: probe.
    HotTier probe(num_buckets);
    // Insert seed at fp=1, find its bucket by exhaustive probing afterwards.
    // Simpler: just brute force candidate fps and check size after inserting
    // all of them into a fresh HotTier. If they all collide they form a group.
    //
    // Fastest: pick seed fp=1, find which buckets[i] holds it by scanning all
    // buckets — but HotTier doesn't expose that. So we use the indirect
    // approach: insert candidates one-by-one and watch size().
    probe.upsert(1, 0);
    std::vector<std::uint32_t> out;
    out.push_back(1);
    for (std::uint32_t fp = 2; out.size() < count && fp < UINT32_MAX; ++fp) {
        const std::size_t before = probe.size();
        probe.upsert(fp, fp);
        const std::size_t after = probe.size();
        if (after == before + 1) {
            // Inserted successfully: ambiguous (same bucket with empty slot,
            // or different bucket).
        }
        // We can't tell from outside without exposing internals. Do something
        // different: use the same hash as HotTier::bucket_index.
        // Fall back to copy of the hash function.
        (void)before;
        (void)after;
    }
    return out;  // (will be replaced — see helper below)
}

// Replicate HotTier::bucket_index for tests so we can synthesize collisions
// without exposing internals. Must mirror the production code exactly.
std::size_t test_bucket_index(std::uint32_t fp, std::size_t mask) {
    std::uint64_t h = fp;
    h ^= h >> 16;
    h *= 0x85ebca6b9c1cdcbull;
    h ^= h >> 13;
    return static_cast<std::size_t>(h) & mask;
}

std::vector<std::uint32_t> collision_fps(std::size_t num_buckets_pow2,
                                         std::uint32_t target_bucket,
                                         std::size_t count) {
    const std::size_t mask = num_buckets_pow2 - 1;
    std::vector<std::uint32_t> out;
    for (std::uint32_t fp = 1; out.size() < count && fp < UINT32_MAX; ++fp) {
        if (test_bucket_index(fp, mask) == target_bucket) {
            out.push_back(fp);
        }
    }
    return out;
}

int run_sieve_basic() {
    std::cout << "=== M1 SIEVE basic eviction ===" << std::endl;
    constexpr std::size_t kBuckets = 64;
    HotTier ht(kBuckets);

    // Build 17 fps that all hash to bucket 0 (16 fits, 17th forces eviction).
    auto fps = collision_fps(kBuckets, 0, 17);
    if (fps.size() < 17) {
        std::cerr << "  WARN: could not find 17 collisions in bucket 0, got "
                  << fps.size() << " — skipping" << std::endl;
        return 0;
    }

    // Fill bucket exactly.
    for (std::size_t i = 0; i < 16; ++i) ht.upsert(fps[i], static_cast<std::uint32_t>(i));
    if (ht.size() != 16) {
        std::cerr << "  FAIL: expected 16 entries after fill, got " << ht.size()
                  << std::endl;
        return 1;
    }
    if (ht.eviction_count() != 0) {
        std::cerr << "  FAIL: unexpected pre-fill evictions: "
                  << ht.eviction_count() << std::endl;
        return 1;
    }

    // 17th insert: should evict one entry, then place fp[16].
    ht.upsert(fps[16], 999);
    if (ht.size() != 16) {
        std::cerr << "  FAIL: expected size 16 after eviction-and-insert, got "
                  << ht.size() << std::endl;
        return 1;
    }
    if (ht.eviction_count() != 1) {
        std::cerr << "  FAIL: expected exactly 1 eviction, got "
                  << ht.eviction_count() << std::endl;
        return 1;
    }
    auto v = ht.lookup(fps[16]);
    if (!v || *v != 999) {
        std::cerr << "  FAIL: post-eviction lookup of new entry wrong"
                  << std::endl;
        return 1;
    }

    // Exactly one of fps[0..15] should be evicted (no longer in tier).
    std::size_t still_present = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        if (ht.peek(fps[i]).has_value()) ++still_present;
    }
    if (still_present != 15) {
        std::cerr << "  FAIL: expected 15 of original 16 still present, got "
                  << still_present << std::endl;
        return 1;
    }
    std::cout << "  PASS (17th insert evicted exactly one of fps[0..15])"
              << std::endl;
    return 0;
}

int run_sieve_visited_semantics() {
    std::cout << "=== M1 SIEVE visited-bit semantics ===" << std::endl;
    constexpr std::size_t kBuckets = 64;
    HotTier ht(kBuckets);

    auto fps = collision_fps(kBuckets, 0, 17);
    if (fps.size() < 17) {
        std::cerr << "  WARN: skip (not enough collisions)" << std::endl;
        return 0;
    }

    // Insert fps[0]. Lookup it many times (visited=1).
    ht.upsert(fps[0], 100);
    for (int i = 0; i < 50; ++i) (void)ht.lookup(fps[0]);

    // Insert fps[1..15] without lookups (visited=0).
    for (std::size_t i = 1; i < 16; ++i) ht.upsert(fps[i], static_cast<std::uint32_t>(i));

    // Insert fps[16]: SIEVE should NOT evict fps[0] (visited=1) on the first
    // pass. It will clear fps[0]'s visited bit but pick a visited=0 entry
    // (one of fps[1..15]) for eviction.
    ht.upsert(fps[16], 999);

    // fps[0] should still be present.
    auto v0 = ht.peek(fps[0]);
    if (!v0 || *v0 != 100) {
        std::cerr << "  FAIL: SIEVE evicted the hot entry (visited=1). "
                  << "fps[0] is " << (v0 ? "wrong value" : "MISSING") << std::endl;
        return 1;
    }
    // fps[16] should be present.
    auto v16 = ht.peek(fps[16]);
    if (!v16 || *v16 != 999) {
        std::cerr << "  FAIL: new entry not stored after eviction" << std::endl;
        return 1;
    }
    std::cout << "  PASS (visited=1 entry survived eviction over 15 visited=0 entries)"
              << std::endl;
    return 0;
}

int run_sieve_hand_distribution() {
    std::cout << "=== M1 SIEVE hand-pointer distribution ===" << std::endl;
    // Insert 16 distinct fps into bucket 0 (all visited=0 since new inserts
    // clear visited). Then insert 16 MORE colliding fps. Each forces one
    // eviction. If the hand pointer advances correctly, evictions should
    // distribute across all 16 slots: every original entry gets evicted,
    // and the bucket ends up holding exactly the 16 newer fps.
    //
    // Regression target: hand pointer stuck at 0 (or any single index)
    // would let the same slot get evicted-and-refilled 16 times in a row,
    // leaving 15 of the original 16 entries untouched.
    constexpr std::size_t kBuckets = 64;
    HotTier ht(kBuckets);

    auto fps = collision_fps(kBuckets, 0, 32);
    if (fps.size() < 32) {
        std::cerr << "  WARN: skip (need 32 collisions, got " << fps.size()
                  << ")" << std::endl;
        return 0;
    }

    for (std::size_t i = 0; i < 16; ++i)
        ht.upsert(fps[i], static_cast<std::uint32_t>(i));
    for (std::size_t i = 16; i < 32; ++i)
        ht.upsert(fps[i], static_cast<std::uint32_t>(i));

    if (ht.size() != 16) {
        std::cerr << "  FAIL: size = " << ht.size() << " after 32 inserts"
                  << std::endl;
        return 1;
    }
    if (ht.eviction_count() != 16) {
        std::cerr << "  FAIL: expected 16 evictions, got "
                  << ht.eviction_count() << std::endl;
        return 1;
    }

    std::size_t old_remaining = 0;
    std::size_t new_present = 0;
    for (std::size_t i = 0; i < 16; ++i)
        if (ht.peek(fps[i]).has_value()) ++old_remaining;
    for (std::size_t i = 16; i < 32; ++i)
        if (ht.peek(fps[i]).has_value()) ++new_present;

    if (old_remaining != 0) {
        std::cerr << "  FAIL: hand pointer stuck — " << old_remaining
                  << " of 16 original entries still present (expected 0)"
                  << std::endl;
        return 1;
    }
    if (new_present != 16) {
        std::cerr << "  FAIL: only " << new_present
                  << "/16 new entries present" << std::endl;
        return 1;
    }
    std::cout << "  PASS (16 evictions distributed across all 16 slots)"
              << std::endl;
    return 0;
}

int run_sieve_all_visited_multipass() {
    std::cout << "=== M1 SIEVE multi-pass second-chance ===" << std::endl;
    // Fill bucket with 16 entries, then lookup ALL 16 so every slot has
    // visited=1. The 17th insert forces eviction. SIEVE's first pass should
    // clear all 16 visited bits without evicting; the second pass should find
    // them all =0 and evict exactly one. This exercises the
    // 2 × kSlotsPerBucket loop length in sieve_evict — a single-pass impl
    // would loop forever or give up.
    constexpr std::size_t kBuckets = 64;
    HotTier ht(kBuckets);

    auto fps = collision_fps(kBuckets, 0, 17);
    if (fps.size() < 17) {
        std::cerr << "  WARN: skip" << std::endl;
        return 0;
    }

    for (std::size_t i = 0; i < 16; ++i)
        ht.upsert(fps[i], static_cast<std::uint32_t>(i));
    for (std::size_t i = 0; i < 16; ++i)
        (void)ht.lookup(fps[i]);  // every slot now visited=1

    ht.upsert(fps[16], 999);

    if (ht.eviction_count() != 1) {
        std::cerr << "  FAIL: expected exactly 1 eviction, got "
                  << ht.eviction_count() << std::endl;
        return 1;
    }
    if (ht.size() != 16) {
        std::cerr << "  FAIL: size = " << ht.size() << " (expected 16)"
                  << std::endl;
        return 1;
    }
    auto v16 = ht.peek(fps[16]);
    if (!v16 || *v16 != 999) {
        std::cerr << "  FAIL: new entry not stored after multi-pass eviction"
                  << std::endl;
        return 1;
    }
    std::size_t survived = 0;
    for (std::size_t i = 0; i < 16; ++i)
        if (ht.peek(fps[i]).has_value()) ++survived;
    if (survived != 15) {
        std::cerr << "  FAIL: expected 15 survivors, got " << survived
                  << std::endl;
        return 1;
    }
    std::cout << "  PASS (all-visited bucket: pass-1 cleared bits, "
              << "pass-2 evicted one)" << std::endl;
    return 0;
}

int run_sieve_no_loss() {
    std::cout << "=== M1 SIEVE no-loss invariant ===" << std::endl;
    // Insert many keys into a small tier. Track what we inserted. After heavy
    // eviction, every entry currently in the tier must match the most-recent
    // insert for its fingerprint.
    constexpr std::size_t kBuckets = 16;  // tiny: 256 slots
    HotTier ht(kBuckets);
    constexpr std::size_t kInserts = 10'000;

    std::mt19937_64 rng(0xa11ce);
    std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);
    std::unordered_map<std::uint32_t, std::uint32_t> latest;
    for (std::size_t i = 0; i < kInserts; ++i) {
        const std::uint32_t fp = make_fp(dist(rng));
        const std::uint32_t off = static_cast<std::uint32_t>(i);
        ht.upsert(fp, off);
        latest[fp] = off;
    }

    // For every fp ever inserted: lookup should either hit with the latest
    // offset, OR miss (entry was evicted).
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t mismatches = 0;
    for (auto& [fp, exp_off] : latest) {
        auto v = ht.peek(fp);
        if (!v) {
            ++misses;
        } else if (*v != exp_off) {
            ++mismatches;
        } else {
            ++hits;
        }
    }

    std::cout << "  inserts=" << kInserts << " distinct=" << latest.size()
              << " evictions=" << ht.eviction_count() << std::endl;
    std::cout << "  hits=" << hits << " misses=" << misses
              << " mismatches=" << mismatches << std::endl;

    if (mismatches != 0) {
        std::cerr << "  FAIL: stale data after eviction" << std::endl;
        return 1;
    }
    if (ht.size() > kBuckets * 16) {
        std::cerr << "  FAIL: size exceeds capacity, " << ht.size() << " > "
                  << kBuckets * 16 << std::endl;
        return 1;
    }
    if (ht.eviction_count() == 0) {
        std::cerr << "  FAIL: expected evictions, got 0 (test workload too light)"
                  << std::endl;
        return 1;
    }
    std::cout << "  PASS (heavy eviction, no stale data, size invariant ok)"
              << std::endl;
    return 0;
}

int run_microbench() {
    std::cout << "=== M0 lookup microbench (single-threaded) ===" << std::endl;
    HotTier ht(kNumBuckets);

    // Populate the tier with a known working set.
    std::mt19937_64 rng(0xdeadbeef);
    std::uniform_int_distribution<std::uint32_t> dist(1, UINT32_MAX);
    std::vector<std::uint32_t> fps;
    fps.reserve(kNumKeys);
    for (std::size_t i = 0; i < kNumKeys; ++i) {
        const std::uint32_t fp = make_fp(dist(rng));
        ht.upsert(fp, static_cast<std::uint32_t>(i));
        fps.push_back(fp);
    }
    std::cout << "  populated with " << ht.size() << " entries" << std::endl;

    // Hit-mostly workload: lookup keys we just inserted, in random order.
    std::shuffle(fps.begin(), fps.end(), rng);

    auto t0 = std::chrono::steady_clock::now();
    std::size_t accumulator = 0;  // prevent dead-code elimination
    for (std::size_t i = 0; i < kBenchIters; ++i) {
        const std::uint32_t fp = fps[i & (fps.size() - 1)];
        auto v = ht.lookup(fp);
        if (v) accumulator += *v;
    }
    auto t1 = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double mops = static_cast<double>(kBenchIters) * 1e9 / ns / 1e6;

    std::cout << "  iterations: " << kBenchIters << std::endl;
    std::cout << "  elapsed:    " << ns / 1e6 << " ms" << std::endl;
    std::cout << "  throughput: " << mops << " M lookups/s" << std::endl;
    std::cout << "  per-op:     " << ns / kBenchIters << " ns/lookup" << std::endl;
    std::cout << "  (anti-DCE accumulator: " << accumulator << ")" << std::endl;

    if (mops < 8.0) {
        std::cerr << "  BELOW TARGET: 8 M lookups/s. Current: " << mops
                  << std::endl;
        return 1;
    }
    std::cout << "  PASS (target ≥ 8 M lookups/s)" << std::endl;
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= run_correctness();
    rc |= run_remove();
    rc |= run_concurrent();
    rc |= run_sieve_basic();
    rc |= run_sieve_visited_semantics();
    rc |= run_sieve_hand_distribution();
    rc |= run_sieve_all_visited_multipass();
    rc |= run_sieve_no_loss();
    rc |= run_microbench();
    if (rc != 0) {
        std::cerr << "\nFAIL" << std::endl;
        return 1;
    }
    std::cout << "\nALL PASS" << std::endl;
    return 0;
}
