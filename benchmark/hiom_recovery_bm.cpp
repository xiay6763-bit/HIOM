// HiOM recovery benchmark (M6.5).
//
// Measures the open-time win from skipping Viper::recover_database()
// when HiOM is authoritative for the index. The same prefill workload
// is used for both modes (HiOM-mediated, so both ColdTier and CCEH
// are populated at close time); the asymmetry is purely in the
// open() path.
//
// Modes (per dataset size):
//   - Baseline: Viper::open() with default config -> recover_database()
//     walks every VPage, rebuilds CCEH. Time = O(all_records).
//   - HiOM:     Viper::open(skip_recovery=true) skips that rebuild;
//     HiOM constructor does a tail scan from
//     [checkpoint.vpage_frontier - 1, current_block) and upserts into
//     ColdTier idempotently (M6 mechanism). Time = O(tail).
//
// The benchmark prints a table:
//   N      baseline_ms   hiom_ms   speedup
//
// Sanity check: after the HiOM open, do 100 random key lookups via
// HiOM's Client::get; all must hit. If any miss, the speedup number
// is meaningless and we abort.
//
// Why not Google Benchmark: this is a one-shot recovery measurement
// per dataset size, not a microbench. Stopwatch + printf is enough
// for the M6.5 deliverable; promote to GBench when wired into M7.
//
// Why not ViperFixture: that fixture pre-supposes a Viper-only DB,
// no ColdTier or Checkpoint. We need HiOM-mediated prefill so both
// the baseline and HiOM modes start from a state where ColdTier is
// fully populated.

#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr const char* kPoolDir = "/pmem0/viper_hiom_recbm";
constexpr const char* kColdPoolFile
    = "/pmem0/viper_hiom_recbm_cold/cold.bin";
constexpr const char* kColdPoolDir = "/pmem0/viper_hiom_recbm_cold";
constexpr const char* kCheckpointFile
    = "/pmem0/viper_hiom_recbm_chkpt/chkpt.bin";
constexpr const char* kCheckpointDir = "/pmem0/viper_hiom_recbm_chkpt";

// Generous pool sizing so 100M entries (1.6 GiB raw + bookkeeping)
// fit. 8 GiB covers up to ~400M (uint64_t, uint64_t) pairs.
constexpr std::size_t kPoolSize = 8ULL << 30;

// Per-N pool sizing keeps the mmap cost proportional to the working
// set so the recovery measurement isn't dominated by 8 GiB of cold
// page-table setup. Pool size must be a multiple of 1 GiB
// (ViperConfig::dax_alignment default).
std::size_t pool_size_for(std::size_t N) {
    constexpr std::size_t kGiB = 1ULL << 30;
    // ~16 B raw per entry, plus bookkeeping. 3× margin keeps headroom.
    const std::size_t needed = N * 16 * 3;
    // Round up to GiB.
    const std::size_t gibs = (needed + kGiB - 1) / kGiB;
    return std::max<std::size_t>(gibs, 1) * kGiB;
}

// Returns floor of log2 such that (1ULL << r) >= n.
std::size_t pow2_ceil_log2(std::size_t n) {
    std::size_t r = 0;
    std::size_t v = 1;
    while (v < n) { v <<= 1; ++r; }
    return r;
}

// Size HotTier so capacity (≈ 16 slots per bucket) is at least 2× N.
// This keeps SIEVE eviction quiet during the prefill so per-put cost
// stays at the M0 baseline (~50 ns) instead of paying inline-flush
// back-pressure when buckets fill with PINNED slots. The recovery
// benchmark cares about correctness of the prefill (so all N reach
// ColdTier) and wall-clock prefill time (cosmetic) but not its
// per-op cost. Over-sized hot is fine: ~24 B per bucket × 16M
// buckets = ~384 MB DRAM at N=100M. Acceptable.
std::size_t hot_buckets_for(std::size_t N) {
    // capacity = buckets * 16; want capacity ≥ 2*N -> buckets ≥ N/8
    const std::size_t target = std::max<std::size_t>(N / 8, 1ULL << 10);
    return 1ULL << pow2_ceil_log2(target);
}

using ViperT = viper::Viper<std::uint64_t, std::uint64_t>;
using HiOMT  = viper::hiom::HiOM<std::uint64_t, std::uint64_t>;

void cleanup_dir(const char* dir, const char* expected_prefix) {
    if (std::string(dir).find(expected_prefix) != 0) {
        std::cerr << "Refusing to clean unfamiliar path: " << dir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
}

void cleanup_all() {
    // Pool dir is created by Viper::create itself; just remove it.
    if (std::string(kPoolDir).find("/pmem0/viper_hiom_recbm") != 0) {
        std::cerr << "Refusing to clean unfamiliar pool: " << kPoolDir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(kPoolDir);
    cleanup_dir(kColdPoolDir, "/pmem0/viper_hiom_recbm_cold");
    cleanup_dir(kCheckpointDir, "/pmem0/viper_hiom_recbm_chkpt");
}

// Prefill N entries via raw Viper + direct ColdTier upsert. We
// deliberately bypass the HiOM commit-buffer / flusher / HotTier
// machinery — those are designed for steady-state throughput at
// concurrent client scale; here we just need a correct, fast prefill
// that populates all three persistent layers (Viper VPages,
// Viper's CCEH, ColdTier) and then writes a sealed Checkpoint
// record. The choice of prefill path doesn't bias the recovery
// measurement: the open() time we measure is purely a function of
// what's on PM, not how it got there.
//
// At close, the PM state is:
//   - Viper VPages: all N (key, value) records.
//   - Viper CCEH: all N (key, offset) entries (used by the baseline
//     open() — it rebuilds CCEH on every open).
//   - ColdTier: all N (fp64, offset) entries (used by HiOM open()).
//   - Checkpoint: a sealed record with vpage_frontier = the post-
//     prefill cursor; M6 tail scan covers [frontier-1, current).
void prefill(std::size_t N, std::uint64_t seed) {
    auto viper = ViperT::create(kPoolDir, pool_size_for(N));
    constexpr std::size_t kNumRegions = 32;
    constexpr std::size_t kEntriesPerBucket = 7;
    const std::size_t main_buckets
        = std::max<std::size_t>(
            8192,
            (N / kNumRegions / kEntriesPerBucket) * 3);
    const std::size_t overflow_slots = main_buckets * 2;
    auto cold  = viper::hiom::ColdTier::create(
        kColdPoolFile, main_buckets, overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);

    auto vclient = viper->get_client();

    std::mt19937_64 rng(seed);
    const auto pf_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint64_t key = i + 1;
        vclient.put(key, rng());
        const auto off = vclient.hiom_peek_offset(key);
        cold->upsert(viper::hiom::key_fingerprint64(key), off);
        if ((i & ((1 << 20) - 1)) == 0 && i > 0) {
            std::printf("    prefilled %zu / %zu\r", i, N);
            std::fflush(stdout);
        }
    }
    const auto pf_end = std::chrono::steady_clock::now();
    const double pf_ms = std::chrono::duration<double, std::milli>(
                            pf_end - pf_start).count();
    const std::size_t cold_size = cold->approx_size();
    std::printf("    prefill: %.1f ms (%.1f M ops/s)  cold=%zu/%zu\n",
                pf_ms, N / (pf_ms / 1000.0) / 1e6,
                cold_size, N);
    if (cold_size != N) {
        std::cerr << "  FAIL: ColdTier holds " << cold_size
                  << " entries; expected " << N << std::endl;
        std::exit(1);
    }

    // Seal a final checkpoint so HiOM open's tail scan has a
    // recent frontier to work from. Without this, read_valid()
    // would return nullopt and the tail scan would walk the whole
    // pool — making the M6.5 measurement degenerate.
    viper::hiom::CheckpointRecord rec{};
    rec.seq = 1;
    rec.flushed_count = N;
    rec.vpage_frontier = viper->hiom_vpage_frontier();
    rec.cold_size = N;
    chkpt->write(rec);
}

// Time Viper::open() with default ViperConfig — full CCEH rebuild
// via recover_database(). Returns ms.
double time_baseline_open(std::uint8_t recovery_threads) {
    viper::ViperConfig cfg;
    cfg.num_recovery_threads = recovery_threads;
    // skip_recovery defaults false.
    const auto t0 = std::chrono::steady_clock::now();
    auto viper = ViperT::open(kPoolDir, cfg);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Time HiOM open path: Viper::open(skip_recovery=true) + ColdTier
// + Checkpoint + HiOM ctor with tail_scan=true. Returns ms.
// Also runs a sanity read pass to confirm ColdTier is authoritative.
double time_hiom_open(std::size_t N, std::uint8_t recovery_threads) {
    const auto t0 = std::chrono::steady_clock::now();
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    auto viper = ViperT::open(kPoolDir, vcfg);
    auto cold  = viper::hiom::ColdTier::open(kColdPoolFile);
    auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
    HiOMT::RecoveryConfig rcfg;
    rcfg.tail_scan = true;
    rcfg.recovery_threads = recovery_threads;
    HiOMT hiom(*viper, hot_buckets_for(N), cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(),
               HiOMT::CheckpointConfig{}, rcfg);
    const auto t1 = std::chrono::steady_clock::now();
    const double open_ms
        = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Sanity pass: 100 random key reads must all hit. If they don't,
    // the speedup measurement is meaningless (we'd be measuring open
    // time of a corrupt index).
    auto client = hiom.get_client();
    std::mt19937_64 rng(0xfeed);
    std::size_t hits = 0;
    constexpr std::size_t kSanity = 100;
    for (std::size_t i = 0; i < kSanity; ++i) {
        const std::uint64_t k
            = (rng() % N) + 1;  // keys are i+1 for i in [0, N)
        std::uint64_t got = 0;
        if (client.get(k, &got)) ++hits;
    }
    if (hits != kSanity) {
        std::cerr << "  FAIL: HiOM open sanity reads " << hits
                  << " / " << kSanity << std::endl;
        std::exit(1);
    }
    std::printf("    sanity reads %zu/%zu\n", hits, kSanity);
    return open_ms;
}

void run_one(std::size_t N, std::uint8_t threads) {
    std::printf("\n=== N=%zu  recovery_threads=%u ===\n",
                N, static_cast<unsigned>(threads));
    cleanup_all();
    prefill(N, /*seed=*/0xc4ec0700);

    const double baseline_ms = time_baseline_open(threads);
    std::printf("    baseline_ms = %.1f\n", baseline_ms);

    const double hiom_ms = time_hiom_open(N, threads);
    std::printf("    hiom_ms     = %.1f\n", hiom_ms);

    const double speedup = baseline_ms / hiom_ms;
    std::printf("    speedup     = %.1fx\n", speedup);
}

}  // namespace

int main(int argc, char** argv) {
    // Default sweep is dev-loop: small N for quick iteration. Pass
    // --full to run the 100M case (takes ~30 min including prefill).
    bool full = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--full") full = true;
    }

    std::vector<std::size_t> sizes = {1'000'000, 10'000'000};
    if (full) sizes.push_back(100'000'000);

    constexpr std::uint8_t kThreads = 32;

    std::printf("HiOM recovery benchmark (M6.5)\n");
    std::printf("Pool=%s  recovery_threads=%u\n",
                kPoolDir, kThreads);
    if (!full) {
        std::printf("(dev-loop sweep; pass --full for 100M case)\n");
    }

    for (auto N : sizes) run_one(N, kThreads);

    std::printf("\nDone.\n");
    return 0;
}
